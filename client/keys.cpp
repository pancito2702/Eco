#include "keys.hpp"
#include "../common/i18n.hpp"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#include <wincrypt.h>  // DPAPI (CryptProtectData)
#else
#include <unistd.h>
#endif

namespace eco {

namespace {

// Markers at the start of the text (base64 follows)
const char kPmMarker[] = "\x01" "P:";       // old private message (fixed key between identities): read only
const char kRatchetMarker[] = "\x01" "R:";  // private message with Double Ratchet
const char kGroupMarker[] = "\x01" "K:";    // channel, old format without signature: read only
const char kSignedMarker[] = "\x01" "S:";   // canal, firmado
constexpr size_t kMarkerLen = 3;
constexpr size_t kRingMax = 2000;  // sender keys kept to read the history

// Double Ratchet
constexpr uint8_t kRatchetVersion = 2;
constexpr size_t kHdr = 1 + 1 + 8 + 32 + 4 + 4;  // version, flags, session, DH key, pn, n
constexpr uint32_t kMaxSkip = 1000;              // lost messages tolerated in a chain
constexpr size_t kMaxSkipped = 2000;             // skipped keys kept per session
constexpr size_t kMaxSessionsPerPeer = 4;
constexpr size_t kMaxPeers = 1000;
constexpr size_t kPlainCacheMax = 3000;
constexpr size_t kSig = 64;

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

bool unhex(const std::string& s, uint8_t* out, size_t n) {
    if (s.size() != n * 2) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < n; ++i) {
        int a = v(s[2 * i]), b = v(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = uint8_t(a << 4 | b);
    }
    return true;
}

uint64_t idOf(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
    return v;
}

void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (56 - 8 * i)); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i)); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

const uint8_t* u8(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

// Creates the file only if it doesn't exist (and private to the user)
bool writeNew(const std::string& path, const std::string& content) {
#ifdef _WIN32
    int fd = _open(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) return false;
    bool ok = _write(fd, content.data(), unsigned(content.size())) == int(content.size());
    _close(fd);
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    bool ok = ::write(fd, content.data(), content.size()) == ssize_t(content.size());
    ::close(fd);
#endif
    return ok;
}

[[maybe_unused]] void privateFile(const std::string& path) {
#ifndef _WIN32
    ::chmod(path.c_str(), 0600);
#else
    (void)path;
#endif
}

// Writes the whole file at once: either the new one or the previous one remains, never a half-written one
bool replaceFile(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp";
#ifdef _WIN32
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.write(content.data(), std::streamsize(content.size()))) return false;
    }
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    // Created already private (0600): not readable by other users for even an instant
    ::unlink(tmp.c_str());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = ::write(fd, content.data(), content.size()) == ssize_t(content.size());
    ok = ::fsync(fd) == 0 && ok;
    ::close(fd);
    if (!ok) { ::unlink(tmp.c_str()); return false; }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

// Appends a line to a private file (creates it with 0600 permissions if it doesn't exist)
void appendPrivate(const std::string& path, const std::string& line) {
#ifdef _WIN32
    std::ofstream f(path, std::ios::app | std::ios::binary);
    f << line << "\n";
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    std::string l = line + "\n";
    (void)!::write(fd, l.data(), l.size());
    ::close(fd);
    privateFile(path);  // in case it came from a previous version with other permissions
#endif
}

// ---- Double Ratchet primitives (BLAKE2b as KDF, X25519, XChaCha20-Poly1305) ----
void kdfRk(uint8_t rk[32], uint8_t ck[32], const uint8_t dh[32]) {
    uint8_t msg[9 + 32], out[64];
    std::memcpy(msg, "eco-dr-rk", 9);
    std::memcpy(msg + 9, dh, 32);
    crypto_blake2b_keyed(out, 64, rk, 32, msg, sizeof msg);
    std::memcpy(rk, out, 32);
    std::memcpy(ck, out + 32, 32);
    crypto_wipe(out, 64);
    crypto_wipe(msg, sizeof msg);
}

void kdfCk(uint8_t ck[32], uint8_t mk[32]) {
    const uint8_t one = 1, two = 2;
    uint8_t next[32];
    crypto_blake2b_keyed(mk, 32, ck, 32, &one, 1);
    crypto_blake2b_keyed(next, 32, ck, 32, &two, 1);
    std::memcpy(ck, next, 32);
    crypto_wipe(next, 32);
}

bool dh(uint8_t out[32], const uint8_t sec[32], const uint8_t pub[32]) {
    static const uint8_t zero[32] = {0};
    crypto_x25519(out, sec, pub);
    return crypto_verify32(out, zero) != 0;  // low-order public key: rejected
}

void newPair(uint8_t sec[32], uint8_t pub[32]) {
    secureRandom(sec, 32);
    crypto_x25519_public_key(pub, sec);
}

// What is signed: a 64-byte digest (so a large frame is traversed only once)
void sigDigest(uint8_t out[64], const char* label, const uint8_t* ad, size_t adn, const uint8_t* d, size_t n) {
    crypto_blake2b_ctx c;
    crypto_blake2b_init(&c, 64);
    crypto_blake2b_update(&c, reinterpret_cast<const uint8_t*>(label), std::strlen(label) + 1);
    uint8_t l[4];
    put32(l, uint32_t(adn));
    crypto_blake2b_update(&c, l, 4);
    crypto_blake2b_update(&c, ad, adn);
    crypto_blake2b_update(&c, d, n);
    crypto_blake2b_final(&c, out);
}

// Minimal serialization for the sessions file
struct Out {
    std::string b;
    void raw(const void* p, size_t n) { b.append(reinterpret_cast<const char*>(p), n); }
    void u8v(uint8_t v) { b += char(v); }
    void u32v(uint32_t v) { uint8_t t[4]; put32(t, v); raw(t, 4); }
    void u64v(uint64_t v) { uint8_t t[8]; put64(t, v); raw(t, 8); }
};
struct In {
    const uint8_t* p;
    size_t n;
    bool ok = true;
    bool raw(void* o, size_t k) {
        if (!ok || k > n) return ok = false;
        std::memcpy(o, p, k);
        p += k;
        n -= k;
        return true;
    }
    uint8_t u8v() { uint8_t v = 0; raw(&v, 1); return v; }
    uint32_t u32v() { uint8_t t[4] = {0}; raw(t, 4); return get32(t); }
    uint64_t u64v() { uint8_t t[8] = {0}; raw(t, 8); return uint64_t(get32(t)) << 32 | get32(t + 4); }
};

}  // namespace

bool protectLocal(const std::string& plain, std::string& sealed) {
#ifdef _WIN32
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))}, out{};
    if (!CryptProtectData(&in, L"Eco", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    sealed = "dpapi:" + b64encode(std::vector<uint8_t>(out.pbData, out.pbData + out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
#else
    (void)plain;
    (void)sealed;
    return false;
#endif
}

bool unprotectLocal(const std::string& sealed, std::string& plain) {
#ifdef _WIN32
    if (sealed.compare(0, 6, "dpapi:") != 0) return false;
    std::vector<uint8_t> raw;
    if (!b64decode(sealed.data() + 6, sealed.size() - 6, raw) || raw.empty()) return false;
    DATA_BLOB in{DWORD(raw.size()), raw.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    plain.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
#else
    (void)sealed;
    (void)plain;
    return false;
#endif
}

std::string configPath(const std::string& name) {
#ifdef _WIN32
    const char* d = std::getenv("APPDATA");
    return d ? std::string(d) + "\\" + name + ".txt" : name + ".txt";
#else
    const char* d = std::getenv("HOME");
    return d ? std::string(d) + "/." + name : name + ".txt";
#endif
}

void Keys::Ratchet::wipe() {
    crypto_wipe(rk, 32);
    crypto_wipe(dhsSec, 32);
    crypto_wipe(cks, 32);
    crypto_wipe(ckr, 32);
    for (auto& [k, v] : skipped) crypto_wipe(v.data(), 32);
    skipped.clear();
}

Keys::~Keys() {
    crypto_wipe(secret_, 32);
    crypto_wipe(curKey_, 32);
    crypto_wipe(curSign_, 64);
    for (auto& [k, v] : pairs_) crypto_wipe(v.data(), 32);
    for (auto& [k, e] : ring_) crypto_wipe(&e, sizeof e);
    for (auto& [k, ps] : sessions_)
        for (auto& r : ps.list) r.wipe();
}

bool Keys::load(std::string& err) {
    std::lock_guard<std::mutex> l(mu_);
    // Device identity
    // On Windows it is saved protected with DPAPI ("dpapi:..."); old files (plaintext hex) are
    // read and converted. If DPAPI can't open it (another user, another PC) a new one is not created on top.
    std::string path = configPath("eco_identidad"), h;
    bool legacy = false;
    auto read = [&] {
        std::ifstream f(path);
        h.clear();
        if (!(f >> h)) return false;
        std::string raw;
        if (unprotectLocal(h, raw)) {
            bool ok = raw.size() == 32;
            if (ok) std::memcpy(secret_, raw.data(), 32);
            crypto_wipe(&raw[0], raw.size());
            return ok;
        }
        legacy = unhex(h, secret_, 32);
        return legacy;
    };
    auto stored = [&]() -> std::string {
        std::string p(reinterpret_cast<const char*>(secret_), 32), s;
        bool ok = protectLocal(p, s);
        crypto_wipe(&p[0], p.size());
        return ok ? s : hex(secret_, 32);  // without DPAPI (Linux, macOS): 0600 permissions
    };
    if (!read()) {
        std::ifstream probe(path);
        if (probe.good() && probe.peek() != std::ifstream::traits_type::eof()) {
            err = T("Could not read your identity key in ") + path +
                  T(" (was it copied from another user or another PC?). A new one is not created so the current one "
                    "isn't lost.");
            return false;
        }
        probe.close();
        secureRandom(secret_, 32);
        legacy = false;
        if (!writeNew(path, stored() + "\n") && !read()) {
            err = T("Could not save your identity key in ") + path;
            return false;
        }
    } else if (legacy) {
        std::string s = stored();
        if (s.compare(0, 6, "dpapi:") == 0) replaceFile(path, s + "\n");  // the old one gets protected
    }
    if (!h.empty()) crypto_wipe(&h[0], h.size());
    crypto_x25519_public_key(pub_.data(), secret_);
    ringPath_ = configPath("eco_llaves");
    contactsPath_ = configPath("eco_contactos");
    sessionsPath_ = configPath("eco_sesiones");
    cachePath_ = configPath("eco_privados");
    verifiedPath_ = configPath("eco_verificados");

    // Keyring: sender keys seen, to be able to read the history.
    // Line (plaintext): id key [owner-identity [signing-key]]. On disk it is encrypted:
    // "v2 " + base64(nonce | ciphertext | mac); old plaintext lines are read and encrypted.
    const std::string& ringPath = ringPath_;
    std::vector<std::string> lines;
    bool rewrite = false;
    {
        std::ifstream f(ringPath);
        std::string stored, line;
        while (std::getline(f, stored)) {
            if (!stored.empty() && stored.back() == '\r') stored.pop_back();
            if (stored.compare(0, 3, "v2 ") == 0) {
                if (!ringOpen(stored, line)) continue;
            } else {
                line = stored;
                rewrite = true;
            }
            if (line.size() == 81 || line.size() == 146 || line.size() == 211) lines.push_back(line);
        }
    }
    rewrite |= lines.size() > kRingMax + 500;
    if (lines.size() > kRingMax) lines.erase(lines.begin(), lines.end() - kRingMax);
    for (auto& line : lines) {
        uint8_t id[8], key[32], sig[32];
        PubKey owner;
        if (!unhex(line.substr(0, 16), id, 8) || !unhex(line.substr(17, 64), key, 32)) continue;
        bool hasOwner = line.size() >= 146 && unhex(line.substr(82, 64), owner.data(), 32);
        bool hasSig = line.size() == 211 && unhex(line.substr(147, 64), sig, 32);
        addKey(idOf(id), key, hasOwner ? &owner : nullptr, hasSig ? sig : nullptr, nullptr);
        crypto_wipe(key, 32);
    }
    if (rewrite) {
        std::string all;
        for (auto& line : lines) all += ringLine(line) + "\n";
        replaceFile(ringPath, all);
    }
    for (auto& line : lines) crypto_wipe(&line[0], line.size());

    // Known and verified contacts
    // Line by line: a damaged (or tampered) line is ignored by itself, without misplacing the following ones
    auto loadPins = [](const std::string& path, std::map<std::string, PubKey>& into) {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream is(line);
            std::string server, nick, key, extra;
            PubKey k;
            if ((is >> server >> nick >> key) && !(is >> extra) && unhex(key, k.data(), 32)) into[server + " " + nick] = k;
        }
    };
    loadPins(contactsPath_, contacts_);
    loadPins(verifiedPath_, verified_);
    loadSessions();
    return true;
}

std::string Keys::ringLine(const std::string& line) {
    uint8_t key[32];
    crypto_blake2b_keyed(key, 32, secret_, 32, reinterpret_cast<const uint8_t*>("eco-at-rest-llaves"), 18);
    std::vector<uint8_t> b(24 + line.size() + kMacSize);
    secureRandom(b.data(), 24);
    crypto_aead_lock(b.data() + 24, b.data() + 24 + line.size(), key, b.data(), nullptr, 0, u8(line), line.size());
    crypto_wipe(key, 32);
    return "v2 " + b64encode(b);
}

bool Keys::ringOpen(const std::string& stored, std::string& line) {
    std::vector<uint8_t> b;
    if (!b64decode(stored.data() + 3, stored.size() - 3, b) || b.size() < 24 + kMacSize + 1) return false;
    uint8_t key[32];
    crypto_blake2b_keyed(key, 32, secret_, 32, reinterpret_cast<const uint8_t*>("eco-at-rest-llaves"), 18);
    line.assign(b.size() - 24 - kMacSize, '\0');
    bool ok = crypto_aead_unlock(reinterpret_cast<uint8_t*>(&line[0]), b.data() + 24 + line.size(), key, b.data(),
                                 nullptr, 0, b.data() + 24, line.size()) == 0;
    crypto_wipe(key, 32);
    crypto_wipe(b.data(), b.size());
    if (!ok) line.clear();
    return ok;
}

// Contact files are "server nick key" separated by spaces. The nick is dictated by the server:
// one with spaces, line breaks or control characters is not saved (it would break the file and, with it,
// the key-change warnings for the contacts on other servers).
static bool storableName(const std::string& s) {
    if (s.empty() || s.size() > 256) return false;
    for (unsigned char c : s)
        if (c <= 0x20 || c == 0x7f) return false;
    return true;
}

// ---------- Verificados ----------
void Keys::saveVerified() {
    std::string all;
    for (auto& [k, v] : verified_) all += k + " " + hex(v.data(), 32) + "\n";
    replaceFile(verifiedPath_, all);
}

void Keys::setVerified(const std::string& server, const std::string& nick, const PubKey& key) {
    if (!storableName(server) || !storableName(nick)) return;
    std::lock_guard<std::mutex> l(mu_);
    verified_[server + " " + lower(nick)] = key;
    saveVerified();
}

void Keys::forgetVerified(const std::string& server, const std::string& nick) {
    std::lock_guard<std::mutex> l(mu_);
    if (verified_.erase(server + " " + lower(nick))) saveVerified();
}

bool Keys::isVerified(const std::string& server, const std::string& nick, const PubKey& key) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = verified_.find(server + " " + lower(nick));
    return it != verified_.end() && it->second == key;
}

bool Keys::blocked(const std::string& server, const std::string& nick, const PubKey& key) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = verified_.find(server + " " + lower(nick));
    return it != verified_.end() && it->second != key;
}

// ---------- Contactos ----------
void Keys::saveContacts() {
    std::string all;
    for (auto& [k, v] : contacts_) all += k + " " + hex(v.data(), 32) + "\n";
    replaceFile(contactsPath_, all);
}

Keys::Seen Keys::remember(const std::string& server, const std::string& nick, const PubKey& key) {
    if (!storableName(server) || !storableName(nick)) return Seen::New;  // not remembered (see storableName)
    std::lock_guard<std::mutex> l(mu_);
    std::string k = server + " " + lower(nick);
    auto it = contacts_.find(k);
    if (it != contacts_.end() && it->second == key) return Seen::Same;
    Seen r = it == contacts_.end() ? Seen::New : Seen::Changed;
    contacts_[k] = key;
    saveContacts();
    return r;
}

bool Keys::lookup(const std::string& server, const std::string& nick, PubKey& key) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = contacts_.find(server + " " + lower(nick));
    if (it == contacts_.end()) return false;
    key = it->second;
    return true;
}

std::string Keys::safetyCode(const PubKey& peer) {
    // Depends only on the two public keys: both get the same number
    uint8_t msg[10 + 64], h[30];
    std::memcpy(msg, "eco-safety", 10);
    const PubKey& a = std::min(pub_, peer);
    const PubKey& b = std::max(pub_, peer);
    std::memcpy(msg + 10, a.data(), 32);
    std::memcpy(msg + 42, b.data(), 32);
    crypto_blake2b(h, sizeof h, msg, sizeof msg);
    std::string s;
    for (int g = 0; g < 6; ++g) {
        uint64_t v = 0;
        for (int i = 0; i < 5; ++i) v = v << 8 | h[g * 5 + i];
        char buf[8];
        std::snprintf(buf, sizeof buf, "%05u", unsigned(v % 100000));
        s += (g ? " " : "") + std::string(buf);
    }
    return s;
}

// ---------- Key between two identities (root of the Double Ratchet and of the key distribution) ----------
bool Keys::pairKey(const PubKey& peer, uint8_t out[32]) {
    auto it = pairs_.find(peer);
    if (it != pairs_.end()) { std::memcpy(out, it->second.data(), 32); return true; }
    uint8_t shared[32];
    if (!dh(shared, secret_, peer.data())) return false;  // invalid public key
    uint8_t msg[8 + 64];
    std::memcpy(msg, "eco-pair", 8);
    const PubKey& a = std::min(pub_, peer);
    const PubKey& b = std::max(pub_, peer);
    std::memcpy(msg + 8, a.data(), 32);
    std::memcpy(msg + 40, b.data(), 32);
    crypto_blake2b_keyed(out, 32, shared, 32, msg, sizeof msg);
    crypto_wipe(shared, 32);
    std::memcpy(pairs_[peer].data(), out, 32);
    return true;
}

bool Keys::isPm(const std::string& s) {
    return s.compare(0, kMarkerLen, kRatchetMarker) == 0 || s.compare(0, kMarkerLen, kPmMarker) == 0;
}
bool Keys::isGroup(const std::string& s) {
    return s.compare(0, kMarkerLen, kSignedMarker) == 0 || s.compare(0, kMarkerLen, kGroupMarker) == 0;
}

// ---------- Privados: Double Ratchet ----------
// Message: header [u8 version][u8 flags: 1 = sent by whoever started the session][8 session]
//          [32 sender DH key][u32 pn][u32 n], then ciphertext and MAC. The header is authenticated.
std::string Keys::sealPm(const std::string& plain, const PubKey& peer, const std::string& ad) {
    std::lock_guard<std::mutex> l(mu_);
    return ratchetSeal(plain, peer, ad);
}

bool Keys::openPm(const std::string& sealed, const PubKey& peer, const std::string& ad, std::string& plain, bool live,
                  bool* replay) {
    if (replay) *replay = false;
    std::vector<uint8_t> buf;
    if (!isPm(sealed) || !b64decode(sealed.data() + kMarkerLen, sealed.size() - kMarkerLen, buf)) return false;
    std::lock_guard<std::mutex> l(mu_);
    if (sealed.compare(0, kMarkerLen, kRatchetMarker) == 0) {
        // Already read before (or sent by me): its keys no longer exist, it is read from the local copy.
        // But if one already read "arrives now", it is the server replaying it: it is not shown as new.
        if (cachedPlain(sealed + '\0' + ad + std::string(reinterpret_cast<const char*>(peer.data()), 32), plain)) {
            if (!live) return true;
            crypto_wipe(&plain[0], plain.size());
            plain.clear();
            if (replay) *replay = true;
            return false;
        }
        if (!ratchetOpen(buf, peer, ad, plain)) return false;
        cachePlain(sealed + '\0' + ad + std::string(reinterpret_cast<const char*>(peer.data()), 32), plain);
        return true;
    }
    // Old format: fixed key between the two identities. Nobody sends it any more and it has no protection
    // against replays: it is only read in the history, never as a new message
    if (live) return false;
    if (buf.size() < 24 + kMacSize) return false;
    uint8_t key[32];
    if (!pairKey(peer, key)) return false;
    size_t n = buf.size() - 24 - kMacSize;
    plain.resize(n);
    uint8_t dummy;
    uint8_t* out = n ? reinterpret_cast<uint8_t*>(&plain[0]) : &dummy;
    bool ok = crypto_aead_unlock(out, buf.data() + 24 + n, key, buf.data(), u8(ad), ad.size(), buf.data() + 24, n) == 0;
    crypto_wipe(key, 32);
    return ok;
}

std::string Keys::ratchetSeal(const std::string& plain, const PubKey& peer, const std::string& ad) {
    auto& ps = sessions_[peer];
    Ratchet* s = nullptr;
    if (ps.hasActive)
        for (auto& r : ps.list)
            if (std::memcmp(r.sid, ps.active, 8) == 0 && r.hasCks) s = &r;
    if (!s) {
        // New session. Root = X25519(my identity, theirs); the first ratchet step adds a
        // my ephemeral key against their identity (like X3DH without prekeys)
        Ratchet r;
        secureRandom(r.sid, 8);
        r.initiator = true;
        uint8_t d[32];
        if (!pairKey(peer, r.rk)) return "";
        newPair(r.dhsSec, r.dhsPub);
        std::memcpy(r.dhr, peer.data(), 32);
        r.hasDhr = true;
        if (!dh(d, r.dhsSec, r.dhr)) return "";
        kdfRk(r.rk, r.cks, d);
        crypto_wipe(d, 32);
        r.hasCks = true;
        if (ps.list.size() >= kMaxSessionsPerPeer) {
            auto old = std::min_element(ps.list.begin(), ps.list.end(),
                                        [](const Ratchet& a, const Ratchet& b) { return a.lastUse < b.lastUse; });
            old->wipe();
            ps.list.erase(old);
        }
        ps.list.push_back(std::move(r));
        s = &ps.list.back();
        std::memcpy(ps.active, s->sid, 8);
        ps.hasActive = true;
    }
    uint8_t mk[32];
    kdfCk(s->cks, mk);
    std::vector<uint8_t> buf(kHdr + plain.size() + kMacSize);
    buf[0] = kRatchetVersion;
    buf[1] = s->initiator ? 1 : 0;
    std::memcpy(&buf[2], s->sid, 8);
    std::memcpy(&buf[10], s->dhsPub, 32);
    put32(&buf[42], s->pn);
    put32(&buf[46], s->ns);
    s->ns++;
    s->lastUse = ++useClock_;
    std::string fullAd(reinterpret_cast<const char*>(buf.data()), kHdr);
    fullAd += ad;
    static const uint8_t nonce[24] = {0};  // each message key is used only once
    crypto_aead_lock(buf.data() + kHdr, buf.data() + kHdr + plain.size(), mk, nonce, u8(fullAd), fullAd.size(),
                     u8(plain), plain.size());
    crypto_wipe(mk, 32);
    // Each message key is single-use (the nonce is fixed). If the advanced state doesn't reach the
    // disk and the program restarts, the chain would go back and the SAME key would encrypt a different
    // message, which breaks the encryption of both. Without saved state, the message doesn't go out.
    if (!saveSessions()) {
        crypto_wipe(buf.data(), buf.size());
        return "";
    }
    std::string sealed = kRatchetMarker + b64encode(buf);
    cachePlain(sealed + '\0' + ad + std::string(reinterpret_cast<const char*>(peer.data()), 32), plain);
    return sealed;
}

bool Keys::ratchetOpen(const std::vector<uint8_t>& msg, const PubKey& peer, const std::string& ad, std::string& plain) {
    if (msg.size() < kHdr + kMacSize || msg[0] != kRatchetVersion) return false;
    bool fromInitiator = (msg[1] & 1) != 0;
    const uint8_t* sid = &msg[2];
    const uint8_t* dhPub = &msg[10];
    uint32_t pn = get32(&msg[42]), n = get32(&msg[46]);

    auto& ps = sessions_[peer];
    Ratchet* found = nullptr;
    for (auto& r : ps.list)
        if (std::memcmp(r.sid, sid, 8) == 0) found = &r;
    // It works on a copy: if the message is fake or damaged, the state doesn't change
    Ratchet w;
    auto fail = [&] {
        w.wipe();
        if (ps.list.empty()) sessions_.erase(peer);
        return false;
    };
    if (found) {
        if (fromInitiator == found->initiator) return fail();  // it is a message of mine (the server's copy)
        w = *found;
    } else {
        // First message of a session the other one started: I reply with my identity as DH key
        if (!fromInitiator) return fail();
        std::memcpy(w.sid, sid, 8);
        w.initiator = false;
        if (!pairKey(peer, w.rk)) return fail();
        std::memcpy(w.dhsSec, secret_, 32);
        std::memcpy(w.dhsPub, pub_.data(), 32);
    }

    // Receiving chain: advances up to "until" saving the keys of the missing messages
    auto skipTo = [&](uint32_t until) {
        if (!w.hasCkr) return true;
        if (until > w.nr + kMaxSkip) return false;
        while (w.nr < until) {
            std::array<uint8_t, 36> k;
            std::memcpy(k.data(), w.dhr, 32);
            put32(k.data() + 32, w.nr);
            uint8_t mk[32];
            kdfCk(w.ckr, mk);
            std::memcpy(w.skipped[k].data(), mk, 32);
            crypto_wipe(mk, 32);
            w.nr++;
            if (w.skipped.size() > kMaxSkipped) {
                crypto_wipe(w.skipped.begin()->second.data(), 32);
                w.skipped.erase(w.skipped.begin());
            }
        }
        return true;
    };

    uint8_t mk[32];
    std::array<uint8_t, 36> key;
    std::memcpy(key.data(), dhPub, 32);
    put32(key.data() + 32, n);
    auto sk = w.skipped.find(key);
    if (sk != w.skipped.end()) {
        std::memcpy(mk, sk->second.data(), 32);
        crypto_wipe(sk->second.data(), 32);
        w.skipped.erase(sk);
    } else {
        if (!w.hasDhr || std::memcmp(dhPub, w.dhr, 32) != 0) {
            // DH step: the other side has a new key. The previous chain is closed and two new ones are derived
            if (!skipTo(pn)) return fail();
            std::memcpy(w.dhr, dhPub, 32);
            w.hasDhr = true;
            w.pn = w.ns;
            w.ns = 0;
            w.nr = 0;
            uint8_t d[32];
            if (!dh(d, w.dhsSec, w.dhr)) return fail();
            kdfRk(w.rk, w.ckr, d);
            w.hasCkr = true;
            newPair(w.dhsSec, w.dhsPub);
            if (!dh(d, w.dhsSec, w.dhr)) return fail();
            kdfRk(w.rk, w.cks, d);
            w.hasCks = true;
            crypto_wipe(d, 32);
        }
        if (!skipTo(n) || w.nr != n) return fail();  // n < nr: repetido
        kdfCk(w.ckr, mk);
        w.nr++;
    }

    size_t len = msg.size() - kHdr - kMacSize;
    std::string fullAd(reinterpret_cast<const char*>(msg.data()), kHdr);
    fullAd += ad;
    static const uint8_t nonce[24] = {0};
    plain.resize(len);
    uint8_t dummy;
    uint8_t* out = len ? reinterpret_cast<uint8_t*>(&plain[0]) : &dummy;
    bool ok = crypto_aead_unlock(out, msg.data() + kHdr + len, mk, nonce, u8(fullAd), fullAd.size(),
                                 msg.data() + kHdr, len) == 0;
    crypto_wipe(mk, 32);
    if (!ok) {
        crypto_wipe(out, len);
        plain.clear();
        return fail();
    }
    // Valid: the new state is saved and this becomes the session used to reply
    w.confirmed = true;
    w.lastUse = ++useClock_;
    if (found) {
        found->wipe();
        *found = std::move(w);
    } else {
        if (ps.list.size() >= kMaxSessionsPerPeer) {
            auto old = std::min_element(ps.list.begin(), ps.list.end(),
                                        [](const Ratchet& a, const Ratchet& b) { return a.lastUse < b.lastUse; });
            old->wipe();
            ps.list.erase(old);
        }
        ps.list.push_back(std::move(w));
    }
    std::memcpy(ps.active, sid, 8);
    ps.hasActive = true;
    saveSessions();
    return true;
}

// Sessions are saved encrypted with a key derived from the identity
bool Keys::saveSessions() {
    // If there are too many contacts, the ones unused for the longest are forgotten
    while (sessions_.size() > kMaxPeers) {
        auto oldest = sessions_.begin();
        uint64_t best = UINT64_MAX;
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            uint64_t last = 0;
            for (auto& r : it->second.list) last = std::max(last, r.lastUse);
            if (last < best) { best = last; oldest = it; }
        }
        for (auto& r : oldest->second.list) r.wipe();
        sessions_.erase(oldest);
    }
    Out o;
    o.raw("ECR1", 4);
    o.u64v(useClock_);
    o.u32v(uint32_t(sessions_.size()));
    for (auto& [peer, ps] : sessions_) {
        o.raw(peer.data(), 32);
        o.u8v(ps.hasActive);
        o.raw(ps.active, 8);
        o.u8v(uint8_t(ps.list.size()));
        for (auto& r : ps.list) {
            o.raw(r.sid, 8);
            o.u8v(uint8_t(r.initiator | r.confirmed << 1 | r.hasDhr << 2 | r.hasCks << 3 | r.hasCkr << 4));
            o.raw(r.rk, 32);
            o.raw(r.dhsSec, 32);
            o.raw(r.dhsPub, 32);
            o.raw(r.dhr, 32);
            o.raw(r.cks, 32);
            o.raw(r.ckr, 32);
            o.u32v(r.ns);
            o.u32v(r.nr);
            o.u32v(r.pn);
            o.u64v(r.lastUse);
            o.u32v(uint32_t(r.skipped.size()));
            for (auto& [k, v] : r.skipped) {
                o.raw(k.data(), 36);
                o.raw(v.data(), 32);
            }
        }
    }
    uint8_t key[32];
    crypto_blake2b_keyed(key, 32, secret_, 32, reinterpret_cast<const uint8_t*>("eco-at-rest-sessions"), 20);
    std::string file(24 + o.b.size() + kMacSize, '\0');
    uint8_t* f = reinterpret_cast<uint8_t*>(&file[0]);
    secureRandom(f, 24);
    crypto_aead_lock(f + 24, f + 24 + o.b.size(), key, f, nullptr, 0, u8(o.b), o.b.size());
    crypto_wipe(key, 32);
    crypto_wipe(&o.b[0], o.b.size());
    return replaceFile(sessionsPath_, file);
}

void Keys::loadSessions() {
    std::ifstream f(sessionsPath_, std::ios::binary);
    std::string file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < 24 + kMacSize) return;
    uint8_t key[32];
    crypto_blake2b_keyed(key, 32, secret_, 32, reinterpret_cast<const uint8_t*>("eco-at-rest-sessions"), 20);
    std::vector<uint8_t> plain(file.size() - 24 - kMacSize);
    const uint8_t* p = u8(file);
    bool ok = crypto_aead_unlock(plain.data(), p + 24 + plain.size(), key, p, nullptr, 0, p + 24, plain.size()) == 0;
    crypto_wipe(key, 32);
    if (!ok) return;
    In in{plain.data(), plain.size()};
    char magic[4];
    if (!in.raw(magic, 4) || std::memcmp(magic, "ECR1", 4) != 0) return;
    useClock_ = in.u64v();
    uint32_t peers = in.u32v();
    for (uint32_t i = 0; i < peers && in.ok; ++i) {
        PubKey peer;
        in.raw(peer.data(), 32);
        PeerSessions ps;
        ps.hasActive = in.u8v() != 0;
        in.raw(ps.active, 8);
        int count = in.u8v();
        for (int j = 0; j < count && in.ok; ++j) {
            Ratchet r;
            in.raw(r.sid, 8);
            uint8_t fl = in.u8v();
            r.initiator = fl & 1;
            r.confirmed = fl & 2;
            r.hasDhr = fl & 4;
            r.hasCks = fl & 8;
            r.hasCkr = fl & 16;
            in.raw(r.rk, 32);
            in.raw(r.dhsSec, 32);
            in.raw(r.dhsPub, 32);
            in.raw(r.dhr, 32);
            in.raw(r.cks, 32);
            in.raw(r.ckr, 32);
            r.ns = in.u32v();
            r.nr = in.u32v();
            r.pn = in.u32v();
            r.lastUse = in.u64v();
            uint32_t sk = in.u32v();
            for (uint32_t k = 0; k < sk && in.ok && k <= kMaxSkipped; ++k) {
                std::array<uint8_t, 36> id;
                std::array<uint8_t, 32> mk;
                in.raw(id.data(), 36);
                in.raw(mk.data(), 32);
                r.skipped[id] = mk;
            }
            if (in.ok) ps.list.push_back(std::move(r));
        }
        if (in.ok) sessions_[peer] = std::move(ps);
    }
    crypto_wipe(plain.data(), plain.size());
}

// ---------- Local copy of read private messages (encrypted with a key derived from the identity) ----------
// Line: digest(16, hex) base64(nonce 24 | ciphertext | mac)
void Keys::loadPlainCache() {
    if (plainCacheLoaded_) return;
    plainCacheLoaded_ = true;
    const std::string& path = cachePath_;
    std::ifstream f(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line))
        if (line.size() > 33 && line[32] == ' ') lines.push_back(std::move(line));
    f.close();
    bool trim = lines.size() > kPlainCacheMax + 500;
    if (lines.size() > kPlainCacheMax) lines.erase(lines.begin(), lines.end() - kPlainCacheMax);
    uint8_t key[32];
    crypto_blake2b_keyed(key, 32, secret_, 32, reinterpret_cast<const uint8_t*>("eco-at-rest-privados"), 20);
    for (auto& l : lines) {
        std::array<uint8_t, 16> h;
        std::vector<uint8_t> b;
        if (!unhex(l.substr(0, 32), h.data(), 16) || !b64decode(l.data() + 33, l.size() - 33, b) || b.size() < 24 + kMacSize)
            continue;
        std::string plain(b.size() - 24 - kMacSize, '\0');
        uint8_t dummy;
        uint8_t* out = plain.empty() ? &dummy : reinterpret_cast<uint8_t*>(&plain[0]);
        if (crypto_aead_unlock(out, b.data() + 24 + plain.size(), key, b.data(), h.data(), 16, b.data() + 24,
                               plain.size()) == 0)
            plainCache_[h] = std::move(plain);
    }
    crypto_wipe(key, 32);
    if (trim) {
        std::string all;
        for (auto& l : lines) all += l + "\n";
        replaceFile(path, all);
    }
}

void Keys::cachePlain(const std::string& id, const std::string& plain) {
    loadPlainCache();
    std::array<uint8_t, 16> h;
    crypto_blake2b(h.data(), 16, u8(id), id.size());
    if (!plainCache_.emplace(h, plain).second) return;
    uint8_t key[32];
    crypto_blake2b_keyed(key, 32, secret_, 32, reinterpret_cast<const uint8_t*>("eco-at-rest-privados"), 20);
    std::vector<uint8_t> b(24 + plain.size() + kMacSize);
    secureRandom(b.data(), 24);
    crypto_aead_lock(b.data() + 24, b.data() + 24 + plain.size(), key, b.data(), h.data(), 16, u8(plain), plain.size());
    crypto_wipe(key, 32);
    appendPrivate(cachePath_, hex(h.data(), 16) + " " + b64encode(b));
    if (plainCache_.size() > kPlainCacheMax + 500) {  // only the recent ones in memory doesn't matter: it will be rebuilt on load
        plainCacheLoaded_ = false;
        plainCache_.clear();
    }
}

bool Keys::cachedPlain(const std::string& id, std::string& plain) {
    loadPlainCache();
    std::array<uint8_t, 16> h;
    crypto_blake2b(h.data(), 16, u8(id), id.size());
    auto it = plainCache_.find(h);
    if (it == plainCache_.end()) return false;
    plain = it->second;
    return true;
}

// ---------- Sender keys ----------
// toDisk: if not null, receives the line (plaintext) to add to the keyring. The caller writes it
// with persistKey() AFTER releasing ringMu_: that way the audio capture thread, which encrypts each frame with
// that lock, never waits for a disk write (before, every join or leave of the channel could
// hold back the voice for a few milliseconds).
void Keys::addKey(uint64_t id, const uint8_t key[32], const PubKey* owner, const uint8_t* sigPub, std::string* toDisk) {
    auto it = ring_.find(id);
    if (it != ring_.end()) {
        // Already had it (from an old file): only what was missing is filled in
        if (!it->second.hasOwner && owner) { it->second.owner = *owner; it->second.hasOwner = true; }
        if (!it->second.hasSig && sigPub) { std::memcpy(it->second.sigPub, sigPub, 32); it->second.hasSig = true; }
        return;
    }
    Entry e;
    crypto_blake2b_keyed(e.text, 32, key, 32, reinterpret_cast<const uint8_t*>("eco-sk-text"), 11);
    crypto_blake2b_keyed(e.voice, 32, key, 32, reinterpret_cast<const uint8_t*>("eco-sk-voice"), 12);
    crypto_blake2b_keyed(e.blob, 32, key, 32, reinterpret_cast<const uint8_t*>("eco-sk-blob"), 11);
    if (owner) { e.owner = *owner; e.hasOwner = true; }
    if (sigPub) { std::memcpy(e.sigPub, sigPub, 32); e.hasSig = true; }
    ring_.emplace(id, e);
    crypto_wipe(&e, sizeof e);
    if (!toDisk) return;
    uint8_t idb[8];
    put64(idb, id);
    *toDisk = hex(idb, 8) + " " + hex(key, 32);
    if (owner) *toDisk += " " + hex(owner->data(), 32);
    if (owner && sigPub) *toDisk += " " + hex(sigPub, 32);
}

void Keys::persistKey(std::string& line) {
    if (line.empty()) return;
    appendPrivate(ringPath_, ringLine(line));  // encrypted on disk
    crypto_wipe(&line[0], line.size());
    line.clear();
}

Keys::Entry* Keys::find(uint64_t id) {
    auto it = ring_.find(id);
    return it == ring_.end() ? nullptr : &it->second;
}

// A key with an owner is only valid if the server says that same owner sends it. Without a known sender
// only the old ownerless keys pass (from previous versions, readable only in the history).
bool Keys::ownerOk(const Entry& e, const PubKey* sender) {
    if (!e.hasOwner) return true;
    return sender && e.owner == *sender;
}

// Each member creates a new key when the channel changes: their last 3 are remembered. What arrives live with
// an older key (e.g. from yesterday, taken from the history) is a reinjection by the server.
void Keys::noteRecent(const PubKey& owner, uint64_t id) {
    auto& v = recent_[owner];
    v.erase(std::remove(v.begin(), v.end(), id), v.end());
    v.push_back(id);
    if (v.size() > 3) v.erase(v.begin());
}

bool Keys::isRecent(const Entry& e, uint64_t id) const {
    if (!e.hasOwner) return false;
    auto it = recent_.find(e.owner);
    return it != recent_.end() && std::find(it->second.begin(), it->second.end(), id) != it->second.end();
}

void Keys::rotate() {
    // The slow part (system randomness and generating the signing pair) is done before taking the lock
    uint8_t id[8], key[32], seed[32], sign[64], signPub[32];
    secureRandom(id, 8);
    secureRandom(key, 32);
    secureRandom(seed, 32);
    crypto_eddsa_key_pair(sign, signPub, seed);  // wipes the seed
    std::string line;
    {
        std::lock_guard<std::mutex> l(ringMu_);
        std::memcpy(curKey_, key, 32);
        std::memcpy(curSign_, sign, 64);
        std::memcpy(curSignPub_, signPub, 32);
        curId_ = idOf(id);
        voiceCtr_ = 0;
        hasCur_ = true;
        addKey(curId_, curKey_, &pub_, curSignPub_, &line);
        noteRecent(pub_, curId_);
    }
    crypto_wipe(key, 32);
    crypto_wipe(sign, 64);
    persistKey(line);
}

// Envelope: [24 nonce] encrypted{ [8 id][32 key][32 signing public key] } [mac]
std::vector<uint8_t> Keys::wrapFor(const PubKey& peer) {
    uint8_t key[32], plain[72], ad[64];
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!pairKey(peer, key)) return {};
    }
    std::lock_guard<std::mutex> l(ringMu_);
    if (!hasCur_) { crypto_wipe(key, 32); return {}; }
    put64(plain, curId_);
    std::memcpy(plain + 8, curKey_, 32);
    std::memcpy(plain + 40, curSignPub_, 32);
    std::memcpy(ad, pub_.data(), 32);       // from me...
    std::memcpy(ad + 32, peer.data(), 32);  // ...to you
    std::vector<uint8_t> out(24 + sizeof plain + kMacSize);
    secureRandom(out.data(), 24);
    crypto_aead_lock(out.data() + 24, out.data() + 24 + sizeof plain, key, out.data(), ad, 64, plain, sizeof plain);
    crypto_wipe(key, 32);
    crypto_wipe(plain, sizeof plain);
    return out;
}

bool Keys::unwrapFrom(const PubKey& peer, const uint8_t* p, size_t n) {
    // 40 = old format (without signing key)
    if (n != 24 + 40 + kMacSize && n != 24 + 72 + kMacSize) return false;
    size_t len = n - 24 - kMacSize;
    uint8_t key[32], plain[72], ad[64];
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!pairKey(peer, key)) return false;
    }
    std::memcpy(ad, peer.data(), 32);
    std::memcpy(ad + 32, pub_.data(), 32);
    bool ok = crypto_aead_unlock(plain, p + 24 + len, key, p, ad, 64, p + 24, len) == 0;
    crypto_wipe(key, 32);
    // The envelope is encrypted with the key between identities: the sender key really is from "peer"
    std::string line;
    if (ok) {
        std::lock_guard<std::mutex> l(ringMu_);
        uint64_t id = idOf(plain);
        addKey(id, plain + 8, &peer, len == 72 ? plain + 40 : nullptr, &line);
        // It only counts as recent if it really is from "peer" (a key already known from someone else isn't stolen)
        Entry* e = find(id);
        if (e && e->hasOwner && e->owner == peer) noteRecent(peer, id);
    }
    crypto_wipe(plain, sizeof plain);
    persistKey(line);
    return ok;
}

// Texto: [8 id][24 nonce][cifrado][mac][64 firma]
std::string Keys::sealText(const std::string& plain, const std::string& ad) {
    std::lock_guard<std::mutex> l(ringMu_);  // channel keys: never wait for the disk write of private messages
    Entry* e = hasCur_ ? find(curId_) : nullptr;
    if (!e) return "";
    size_t body = 32 + plain.size() + kMacSize;
    std::vector<uint8_t> buf(body + kSig);
    put64(buf.data(), curId_);
    secureRandom(buf.data() + 8, 24);
    crypto_aead_lock(buf.data() + 32, buf.data() + 32 + plain.size(), e->text, buf.data() + 8, u8(ad), ad.size(),
                     u8(plain), plain.size());
    uint8_t dg[64];
    sigDigest(dg, "eco-sig-text", u8(ad), ad.size(), buf.data(), body);
    crypto_eddsa_sign(buf.data() + body, curSign_, dg, 64);
    return kSignedMarker + b64encode(buf);
}

bool Keys::openText(const std::string& sealed, const std::string& ad, const PubKey* sender, std::string& plain,
                    bool live) {
    std::vector<uint8_t> buf;
    if (!isGroup(sealed) || !b64decode(sealed.data() + kMarkerLen, sealed.size() - kMarkerLen, buf)) return false;
    bool signedMsg = sealed.compare(0, kMarkerLen, kSignedMarker) == 0;
    size_t tail = signedMsg ? kSig : 0;
    if (buf.size() < 32 + kMacSize + tail) return false;
    if (live && !signedMsg) return false;  // live, only the current signed format
    std::lock_guard<std::mutex> l(ringMu_);  // channel keys: never wait for the disk write of private messages
    uint64_t id = idOf(buf.data());
    Entry* e = find(id);
    if (!e || !ownerOk(*e, sender)) return false;
    std::array<uint8_t, 16> dig{};
    if (live) {
        if (!isRecent(*e, id)) return false;
        crypto_blake2b(dig.data(), 16, buf.data(), buf.size());
        if (seenText_.count(dig)) return false;  // the same message again: replay
    }
    size_t body = buf.size() - tail;
    if (signedMsg) {
        if (!e->hasSig) return false;
        uint8_t dg[64];
        sigDigest(dg, "eco-sig-text", u8(ad), ad.size(), buf.data(), body);
        if (crypto_eddsa_check(buf.data() + body, e->sigPub, dg, 64) != 0) return false;
    } else if (e->hasSig) {
        return false;  // a key that signs never sends unsigned messages: it would be a forgery
    }
    size_t n = body - 32 - kMacSize;
    plain.resize(n);
    uint8_t dummy;
    uint8_t* out = n ? reinterpret_cast<uint8_t*>(&plain[0]) : &dummy;
    if (crypto_aead_unlock(out, buf.data() + 32 + n, e->text, buf.data() + 8, u8(ad), ad.size(), buf.data() + 32, n) != 0)
        return false;
    if (live && seenText_.insert(dig).second) {
        seenOrder_.push_back(dig);
        if (seenOrder_.size() > 8192) {  // the oldest ones are forgotten (their key will no longer be recent)
            seenText_.erase(seenOrder_.front());
            seenOrder_.pop_front();
        }
    }
    return true;
}

// Voice: [8 key id][4 counter][ciphertext][mac]. No signature: 64 more bytes every 20 ms would almost double
// voice traffic; instead it is checked that the key belongs to whoever the server says is talking.
bool Keys::sealVoice(const uint8_t* opus, size_t n, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> l(ringMu_);  // channel keys: never wait for the disk write of private messages
    Entry* e = hasCur_ ? find(curId_) : nullptr;
    if (!e || voiceCtr_ == UINT32_MAX) return false;
    out.resize(12 + n + kMacSize);
    put64(out.data(), curId_);
    put32(out.data() + 8, voiceCtr_++);
    uint8_t nonce[24] = {0};
    std::memcpy(nonce, out.data(), 12);
    crypto_aead_lock(out.data() + 12, out.data() + 12 + n, e->voice, nonce, out.data(), 12, opus, n);
    return true;
}

bool Keys::openVoice(const uint8_t* p, size_t n, const PubKey* sender, std::vector<uint8_t>& opus) {
    if (n < 12 + kMacSize + 1) return false;
    std::lock_guard<std::mutex> l(ringMu_);  // channel keys: never wait for the disk write of private messages
    Entry* e = find(idOf(p));
    // Voice is always live: only with a recent key of whoever is talking
    if (!e || !ownerOk(*e, sender) || !isRecent(*e, idOf(p))) return false;
    size_t len = n - 12 - kMacSize;
    opus.resize(len);
    uint8_t nonce[24] = {0};
    std::memcpy(nonce, p, 12);
    if (crypto_aead_unlock(opus.data(), p + 12 + len, e->voice, nonce, p, 12, p + 12, len) != 0) return false;
    return e->seen.accept(get32(p + 8));
}

// Large data (screen): [8 key id][24 nonce][ciphertext][mac][64 signature]
bool Keys::sealBlob(const uint8_t* p, size_t n, uint8_t ad, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> l(ringMu_);  // channel keys: never wait for the disk write of private messages
    Entry* e = hasCur_ ? find(curId_) : nullptr;
    if (!e) return false;
    size_t body = 32 + n + kMacSize;
    out.resize(body + kSig);
    put64(out.data(), curId_);
    secureRandom(out.data() + 8, 24);
    crypto_aead_lock(out.data() + 32, out.data() + 32 + n, e->blob, out.data() + 8, &ad, 1, p, n);
    uint8_t dg[64];
    sigDigest(dg, "eco-sig-blob", &ad, 1, out.data(), body);
    crypto_eddsa_sign(out.data() + body, curSign_, dg, 64);
    return true;
}

bool Keys::openBlob(const uint8_t* p, size_t n, uint8_t ad, bool sig, const PubKey* sender, std::vector<uint8_t>& out) {
    size_t tail = sig ? kSig : 0;
    if (n < 32 + kMacSize + 1 + tail) return false;
    std::lock_guard<std::mutex> l(ringMu_);  // channel keys: never wait for the disk write of private messages
    Entry* e = find(idOf(p));
    // The screen is always live and always signed
    if (!e || !sig || !ownerOk(*e, sender) || !e->hasSig || !isRecent(*e, idOf(p))) return false;
    size_t body = n - tail;
    if (sig) {
        uint8_t dg[64];
        sigDigest(dg, "eco-sig-blob", &ad, 1, p, body);
        if (crypto_eddsa_check(p + body, e->sigPub, dg, 64) != 0) return false;
    }
    size_t len = body - 32 - kMacSize;
    out.resize(len);
    return crypto_aead_unlock(out.data(), p + 32 + len, e->blob, p + 8, &ad, 1, p + 32, len) == 0;
}

}  // namespace eco
