#include "e2e.hpp"

#include "keys.hpp"

#include <cstdlib>
#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace eco {

namespace {

const char kMarker[] = "\x01" "E2E:";
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string b64encode(const std::vector<uint8_t>& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t v = uint32_t(in[i]) << 16;
        if (i + 1 < in.size()) v |= uint32_t(in[i + 1]) << 8;
        if (i + 2 < in.size()) v |= in[i + 2];
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kB64[v & 63] : '=';
    }
    return out;
}

bool b64decode(const char* p, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = p[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else return false;
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(uint8_t(acc >> bits)); }
    }
    return true;
}

bool E2E::setPassphrase(const std::string& phrase, const uint8_t serverId[32]) {
    // Argon2id (19 MiB, 2 passes): every attempt to guess the phrase is expensive
    const uint32_t kib = 19 * 1024;
    void* work = std::malloc(size_t(kib) * 1024);
    if (!work) return false;
    uint8_t master[32];
    crypto_argon2_config cfg{CRYPTO_ARGON2_ID, kib, 2, 1};
    crypto_argon2_inputs in{reinterpret_cast<const uint8_t*>(phrase.data()), serverId, uint32_t(phrase.size()), 32};
    crypto_argon2(master, 32, work, cfg, in, crypto_argon2_no_extras);
    crypto_wipe(work, size_t(kib) * 1024);  // don't leave anything derived from the phrase in freed memory
    std::free(work);
#ifdef __GLIBC__
    malloc_trim(0);  // give Argon2's temporary memory back to the system
#endif

    std::lock_guard<std::mutex> l(mu_);
    // An independent key for each use
    crypto_blake2b_keyed(textKey_, 32, master, 32, reinterpret_cast<const uint8_t*>("eco-e2e-text"), 12);
    crypto_blake2b_keyed(voiceKey_, 32, master, 32, reinterpret_cast<const uint8_t*>("eco-e2e-voice"), 13);
    crypto_blake2b_keyed(blobKey_, 32, master, 32, reinterpret_cast<const uint8_t*>("eco-e2e-blob"), 12);
    crypto_wipe(master, 32);
    secureRandom(voiceSalt_, 8);
    voiceCtr_ = 0;
    voiceSeen_.clear();
    on_ = true;
    return true;
}

void E2E::clear() {
    std::lock_guard<std::mutex> l(mu_);
    on_ = false;
    crypto_wipe(textKey_, 32);
    crypto_wipe(voiceKey_, 32);
    crypto_wipe(blobKey_, 32);
    voiceSeen_.clear();
}

std::string E2E::check() {
    std::lock_guard<std::mutex> l(mu_);
    if (!on_) return "";
    uint8_t h[3];
    crypto_blake2b_keyed(h, 3, textKey_, 32, reinterpret_cast<const uint8_t*>("eco-e2e-check"), 13);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (uint8_t b : h) { s += hex[b >> 4]; s += hex[b & 15]; }
    return s;
}

bool E2E::isSealed(const std::string& s) { return s.compare(0, sizeof kMarker - 1, kMarker) == 0; }

std::string E2E::sealText(const std::string& plain, const std::string& ad) {
    std::vector<uint8_t> buf(24 + plain.size() + kMacSize);
    secureRandom(buf.data(), 24);  // random 192-bit nonce: no risk of repetition
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!on_) return "";  // the unencrypted text is never returned
        crypto_aead_lock(buf.data() + 24, buf.data() + 24 + plain.size(), textKey_, buf.data(),
                         reinterpret_cast<const uint8_t*>(ad.data()), ad.size(),
                         reinterpret_cast<const uint8_t*>(plain.data()), plain.size());
    }
    return kMarker + b64encode(buf);
}

bool E2E::openText(const std::string& sealed, const std::string& ad, std::string& plain) {
    std::vector<uint8_t> buf;
    size_t skip = sizeof kMarker - 1;
    if (!isSealed(sealed) || !b64decode(sealed.data() + skip, sealed.size() - skip, buf)) return false;
    if (buf.size() < 24 + kMacSize) return false;
    size_t n = buf.size() - 24 - kMacSize;
    plain.resize(n);
    std::lock_guard<std::mutex> l(mu_);
    if (!on_) return false;
    return crypto_aead_unlock(reinterpret_cast<uint8_t*>(&plain[0]), buf.data() + 24 + n, textKey_, buf.data(),
                              reinterpret_cast<const uint8_t*>(ad.data()), ad.size(), buf.data() + 24, n) == 0;
}

bool E2E::sealVoice(const uint8_t* opus, size_t n, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> l(mu_);
    if (!on_ || voiceCtr_ == UINT32_MAX) return false;  // 2^32 packets = years of talking non-stop
    out.resize(12 + n + kMacSize);
    std::memcpy(out.data(), voiceSalt_, 8);
    uint32_t c = voiceCtr_++;
    for (int i = 0; i < 4; ++i) out[8 + i] = uint8_t(c >> (24 - 8 * i));
    uint8_t nonce[24] = {0};
    std::memcpy(nonce, out.data(), 12);
    crypto_aead_lock(out.data() + 12, out.data() + 12 + n, voiceKey_, nonce, out.data(), 12, opus, n);
    return true;
}

bool E2E::openVoice(const uint8_t* p, size_t n, std::vector<uint8_t>& opus) {
    if (n < 12 + kMacSize + 1) return false;
    size_t len = n - 12 - kMacSize;
    opus.resize(len);
    uint8_t nonce[24] = {0};
    std::memcpy(nonce, p, 12);
    std::lock_guard<std::mutex> l(mu_);
    if (!on_) return false;
    if (crypto_aead_unlock(opus.data(), p + 12 + len, voiceKey_, nonce, p, 12, p + 12, len) != 0) return false;
    uint64_t salt = 0;
    uint32_t ctr = 0;
    for (int i = 0; i < 8; ++i) salt = salt << 8 | p[i];
    for (int i = 0; i < 4; ++i) ctr = ctr << 8 | p[8 + i];
    // Many senders: forget any one of them instead of clearing everything (clearing would allow replaying packets)
    if (voiceSeen_.size() > 512 && !voiceSeen_.count(salt)) voiceSeen_.erase(voiceSeen_.begin());
    return voiceSeen_[salt].accept(ctr);  // an authentic but repeated packet is dropped
}

bool E2E::sealBlob(const uint8_t* p, size_t n, uint8_t ad, std::vector<uint8_t>& out) {
    out.resize(24 + n + kMacSize);
    secureRandom(out.data(), 24);
    std::lock_guard<std::mutex> l(mu_);
    if (!on_) return false;
    crypto_aead_lock(out.data() + 24, out.data() + 24 + n, blobKey_, out.data(), &ad, 1, p, n);
    return true;
}

bool E2E::openBlob(const uint8_t* p, size_t n, uint8_t ad, std::vector<uint8_t>& out) {
    if (n < 24 + kMacSize + 1) return false;
    size_t len = n - 24 - kMacSize;
    out.resize(len);
    std::lock_guard<std::mutex> l(mu_);
    if (!on_) return false;
    return crypto_aead_unlock(out.data(), p + 24 + len, blobKey_, p, &ad, 1, p + 24, len) == 0;
}

}  // namespace eco
