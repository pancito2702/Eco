// Eco server: voice, chat and screen sharing, with accounts and history in SQLite.
// A single thread (asio). Voice is only decrypted and re-encrypted for each listener.
// When clients use E2E, the content (text, voice, screen) arrives already encrypted and
// here it is only relayed and stored without being able to read it.
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#ifndef _WIN32
#include <sys/stat.h>  // umask, chmod: eco.db only for the server's user
#endif

#include "../common/version.hpp"
#include "../common/invite.hpp"
#include "../common/protocol.hpp"
#include "../common/i18n.hpp"
#include "../common/secure.hpp"
#include "db.hpp"

using asio::ip::tcp;
using asio::ip::udp;
using namespace eco;

namespace {

struct Config {
    uint16_t port = kDefaultPort;
    std::string serverPass;
    std::string adminPass;
    std::string modPass;
    std::vector<std::string> channels = {"Lobby", "General", i18n::english() ? "Games" : "Juegos", "AFK"};  // first start only
    size_t maxUsers = 64;
    std::string dbFile = "eco.db";
    int retainDays = 0;    // 0 = keep the messages forever
    int joinHistory = 30;  // messages shown when entering a channel
    int backupKeep = 7;    // daily copies of eco.db that are kept (0 = none)
    std::string updateDir; // folder with signed updates for the clients ("" = none offered)
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s) {
    std::istringstream is(s);
    std::vector<std::string> v;
    std::string t;
    while (is >> t) v.push_back(t);
    return v;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// Truncates to at most n bytes without splitting a UTF-8 character (otherwise "?" shows on the other side)
void truncateUtf8(std::string& s, size_t n) {
    if (s.size() <= n) return;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    s.resize(n);
}

bool hasControlChars(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](char c) { return (unsigned char)c < 0x20 || c == 0x7f; });
}

// Removes control characters (e.g. ESC of terminal sequences) from free text
std::string stripControl(std::string s) {
    for (auto& c : s)
        if ((unsigned char)c < 0x20 || c == 0x7f) c = ' ';
    return s;
}

// Nicks and channel names: valid UTF-8, without control, invisible or direction characters
// (bidi, which allow "flipping" a name on screen) and without mixing alphabets that get
// confused with each other (Latin, Greek, Cyrillic): that way "Аdmin" with a Cyrillic A doesn't pass for "Admin".
bool cleanName(const std::string& s, bool allowSpaces) {
    int scripts = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 0x6 && c >= 0xC2) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 0x1E && c <= 0xF4) { cp = c & 0x07; n = 4; }
        else return false;
        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            unsigned char d = (unsigned char)s[i + k];
            if ((d & 0xC0) != 0x80) return false;
            cp = cp << 6 | (d & 0x3F);
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += n;
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0xA0)) return false;  // control, C1 and non-breaking space
        if (cp == ' ' && !allowSpaces) return false;
        if (cp == 0xAD || cp == 0x34F || cp == 0x61C || cp == 0x115F || cp == 0x1160 || cp == 0x17B4 ||
            cp == 0x17B5 || cp == 0x180E || cp == 0x3164 || cp == 0xFEFF || cp == 0xFFA0)
            return false;  // invisibles
        if ((cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202F) || (cp >= 0x205F && cp <= 0x206F) ||
            (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFFF0 && cp <= 0xFFFF) || (cp >= 0xE000 && cp <= 0xF8FF) ||
            cp >= 0xE0000)
            return false;  // odd spaces, bidi marks, variation selectors, private use, tags
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp <= 0x24F) ||
            (cp >= 0x1E00 && cp <= 0x1EFF))
            scripts |= 1;
        else if (cp >= 0x370 && cp <= 0x3FF) scripts |= 2;
        else if (cp >= 0x400 && cp <= 0x52F) scripts |= 4;
    }
    return scripts == 0 || scripts == 1 || scripts == 2 || scripts == 4;
}

bool validNick(const std::string& n) {
    if (n.empty() || n.size() > 24 || !cleanName(n, false)) return false;
    // Prefixes the interface uses for roles and notices (@admin, +mod, "* ", "! ")
    if (std::string("@+*![").find(n[0]) != std::string::npos) return false;
    return n.find(':') == std::string::npos;
}

// Key to group addresses: IPv4 as is; IPv6 by its /64 (any home connection or VPS
// has millions of addresses inside its /64, so counting per address wouldn't slow anything down)
std::string ipKey(const std::string& ip) {
    std::error_code ec;
    auto a = asio::ip::make_address(ip, ec);
    if (ec || !a.is_v6()) return ip;
    auto b = a.to_v6().to_bytes();
    for (size_t i = 8; i < 16; ++i) b[i] = 0;
    return asio::ip::address_v6(b).to_string() + "/64";
}

// Constant-time comparison of secrets (doesn't reveal through timing how many characters match)
bool secretEquals(const std::string& a, const std::string& b) {
    uint8_t ha[32], hb[32];
    crypto_blake2b(ha, 32, reinterpret_cast<const uint8_t*>(a.data()), a.size());
    crypto_blake2b(hb, 32, reinterpret_cast<const uint8_t*>(b.data()), b.size());
    return crypto_verify32(ha, hb) == 0;
}

// Limits against abuse
constexpr uint32_t kMaxPreAuthFrame = 16 * 1024;           // before logging in, only the greeting arrives
constexpr size_t kMaxQueuedBytes = 32u * 1024u * 1024u;    // client that doesn't read: it gets disconnected
constexpr size_t kMaxConnsPerIp = 16;
// New connections per IP (or /64): burst of 20 and then one every 2 s. Each connection costs the server an
// X25519 and a signature: without this brake, connecting and disconnecting in a loop would hog the only thread (voice)
constexpr double kConnBurst = 20, kConnPerSec = 0.5;
// Unidentified connections tolerated at the same time, on top of the user slots. When the cap is reached
// the oldest one still unidentified is closed (a real person identifies in less than 1 s)
constexpr size_t kMaxPendingConns = 256;
constexpr size_t kMaxSealedText = 2000;                    // an encrypted message of 1200 characters takes ~1760
constexpr size_t kKeepReadBuffer = 256 * 1024;             // above this the read buffer is released
constexpr int kMaxHashJobs = 16;                           // passwords being computed at the same time (queue)
constexpr double kMsgBurst = 40, kMsgPerSec = 8;           // messages: burst and sustained rate
constexpr int kMaxHashJobsPerIp = 2;                       // so a single IP doesn't hog the queue
constexpr double kScreenBytesPerSec = 6.0 * 1024 * 1024;   // screen: sustained rate per user
constexpr double kScreenBurst = double(kMaxFrame);         // ...and burst (a large key frame)
constexpr size_t kScreenMinCost = 16 * 1024;               // each frame counts at least this (against thousands of tiny messages)
constexpr double kUdpBurst = 300, kUdpPerSec = 150;        // UDP packets per user: voice (50/s) + screen audio (17/s) + margin
constexpr int kIpFailLimit = 8;                            // failed passwords per IP (or /64) every 5 min
constexpr int kAcctFailLimit = 20;                         // ...and per account every 15 min (against botnets)
constexpr int kPreAuthSeconds = 20;                        // time to identify after connecting
constexpr char kSealedPrefix = 1;                          // first byte of every E2E encrypted text

void logf(const std::string& s);

// Logins, logouts and failed passwords: at most 60 lines per minute. If someone opens thousands of connections,
// the log doesn't grow endlessly (it would fill the disk); after the minute, how many were not written is noted.
struct NoisyLog {
    int used = 0, skipped = 0;
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    void line(const std::string& s) {
        roll();
        if (used < 60) { ++used; logf(s); }
        else ++skipped;
    }
    void roll() {
        auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::minutes(1)) return;
        if (skipped) logf("... y " + std::to_string(skipped) + T(" more logins, logouts or failed passwords in the last minute (not logged one by one)"));
        used = skipped = 0;
        window = now;
    }
} g_noisy;

void logf(const std::string& s) {
    auto t = std::time(nullptr);
    char b[32];
    std::strftime(b, sizeof b, "%H:%M:%S", std::localtime(&t));
    std::cout << "[" << b << "] " << s << std::endl;
}

class Server;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket s, Server& srv) : sock(std::move(s)), server(srv) {}

    tcp::socket sock;
    Server& server;
    uint32_t id = 0;
    std::string nick;
    std::string ip;
    std::string ipGroup;  // ipKey(ip): the IP, or its /64 for IPv6
    Role role = Role::User;
    bool owner = false;     // logged in with the server's admin password: can act on other admins
    bool hashBusy = false;  // has a password being computed (/register, /password...)
    // Deadline to identify: 20 s after connecting; if the server asks for the password, 2 min to type it
    std::chrono::steady_clock::time_point connectedAt = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point authDeadline = connectedAt + std::chrono::seconds(kPreAuthSeconds);
    // Brake on screen sharing (bytes per second)
    double screenTokens = kScreenBurst;
    std::chrono::steady_clock::time_point screenAt = std::chrono::steady_clock::now();
    bool allowScreen(size_t bytes) {
        auto now = std::chrono::steady_clock::now();
        screenTokens = std::min(kScreenBurst,
                                screenTokens + std::chrono::duration<double>(now - screenAt).count() * kScreenBytesPerSec);
        screenAt = now;
        if (screenTokens < double(bytes)) return false;
        screenTokens -= double(bytes);
        return true;
    }
    // Brake on UDP packets: each one is relayed to the whole channel (or to those watching their screen)
    double udpTokens = kUdpBurst;
    std::chrono::steady_clock::time_point udpAt = std::chrono::steady_clock::now();
    bool allowUdp() {
        auto now = std::chrono::steady_clock::now();
        udpTokens = std::min(kUdpBurst, udpTokens + std::chrono::duration<double>(now - udpAt).count() * kUdpPerSec);
        udpAt = now;
        if (udpTokens < 1) return false;
        udpTokens -= 1;
        return true;
    }
    std::string channel;
    bool secure = false;      // key exchange done
    bool authed = false;      // nick aceptado
    bool registered = false;  // has an account with a password
    int authFails = 0;
    bool sharing = false;
    bool serverMuted = false;
    uint32_t watching = 0;
    bool needKey = false;  // the viewer lost frames
    bool hasUdp = false;
    bool tcpVoice = false;      // their UDP doesn't work: voice goes to them over TCP
    bool sharingAudio = false;  // is sending their PC audio with the screen
    std::chrono::steady_clock::time_point lastScreenAudio{};
    std::string clientVersion;  // the one their client reports ("" = older than 2.1)
    udp::endpoint udpEp;
    std::chrono::steady_clock::time_point lastSeen = std::chrono::steady_clock::now();
    bool authBusy = false;  // checking their password on the hash thread
    // Flood brake (token bucket): short bursts yes, a continuous stream no
    double tokens = kMsgBurst;
    std::chrono::steady_clock::time_point tokensAt = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point floodWarned{};
    bool allowMessage() {
        auto now = std::chrono::steady_clock::now();
        tokens = std::min(kMsgBurst, tokens + std::chrono::duration<double>(now - tokensAt).count() * kMsgPerSec);
        tokensAt = now;
        if (tokens < 1) return false;
        tokens -= 1;
        return true;
    }

    uint8_t pubkey[32] = {0};  // client's E2E public key (the server only hands it out)

    SessionKeys keys;
    TcpCipher tx, rx;
    uint64_t udpTx = 0;
    ReplayWindow udpRx;

    void start();
    void send(Msg t, const std::vector<uint8_t>& payload = {}) { sendBody(body(t, payload)); }
    // Encrypts with this session's keys and queues
    void sendBody(const std::vector<uint8_t>& plain) {
        if (!secure || closed_) return;
        sendWire(std::make_shared<std::vector<uint8_t>>(tx.seal(plain.data(), plain.size())));
    }
    void sendWire(std::shared_ptr<std::vector<uint8_t>> data);
    size_t queuedBytes() const { return outBytes_; }
    bool isClosed() const { return closed_; }
    void close();
    void shutdownSocket();

private:
    void readHeader();
    void readBody(uint32_t len);
    void doWrite();
    bool handshake();

    uint8_t hdr_[4];
    std::vector<uint8_t> body_;
    std::deque<std::shared_ptr<std::vector<uint8_t>>> outq_;
    size_t outBytes_ = 0;
    bool closed_ = false, overflow_ = false;
};

using SessionPtr = std::shared_ptr<Session>;

class Server {
public:
    Server(asio::io_context& io, Config cfg, Db& db)
        : io_(io), cfg_(std::move(cfg)), db_(db), acceptor_(io), udp_(io), timer_(io), acceptRetry_(io) {
        loadIdentity();
        channels_ = db_.channels();
        if (channels_.empty()) {
            for (auto& c : cfg_.channels) db_.addChannel(c);
            channels_ = db_.channels();
        }
        bans_ = db_.bans();
        banAccounts_ = db_.sanctionTargets("ban_cuenta");
        muteAccounts_ = db_.sanctionTargets("silencio_cuenta");
        muteIps_ = db_.sanctionTargets("silencio_ip");
        if (cfg_.adminPass.empty()) {
            bool hasAdmin = false;
            for (auto& a : db_.accounts()) hasAdmin |= a.role == Role::Admin;
            if (!hasAdmin) {
                // First time: one-time master password to create the first admin
                static const char cs[] = "abcdefghjkmnpqrstuvwxyz23456789";
                uint8_t rnd[16];
                secureRandom(rnd, sizeof rnd);
                for (uint8_t b : rnd) cfg_.adminPass += cs[b % 31];
                crypto_wipe(rnd, sizeof rnd);
                masterKey_ = true;
                std::cout << T("There is no admin yet. One-time master password: ") << cfg_.adminPass
                          << T("\n  Join, type /register <your-password> and then /login ") << cfg_.adminPass
                          << T("\n  (it stops working as soon as someone uses it)\n");
            }
        }

        // Dual IPv4+IPv6 stack if the system allows it; otherwise only IPv4
        std::error_code ec;
        acceptor_.open(tcp::v6(), ec);
        bool v6 = !ec;
        if (v6) acceptor_.set_option(asio::ip::v6_only(false), ec);
        else acceptor_.open(tcp::v4());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(tcp::endpoint(v6 ? tcp::v6() : tcp::v4(), cfg_.port));
        acceptor_.listen();
        if (v6) {
            udp_.open(udp::v6());
            udp_.set_option(asio::ip::v6_only(false), ec);
            udp_.bind(udp::endpoint(udp::v6(), cfg_.port));
        } else {
            udp_.open(udp::v4());
            udp_.bind(udp::endpoint(udp::v4(), cfg_.port));
        }
        udp_.non_blocking(true);  // ver udpSendTo
        // Somewhat larger system buffers: they withstand bursts with many people talking at once
        udp_.set_option(asio::socket_base::send_buffer_size(256 * 1024), ec);
        udp_.set_option(asio::socket_base::receive_buffer_size(256 * 1024), ec);
        if (int n = db_.prune(cfg_.retainDays)) logf(T("Deleted ") + std::to_string(n) + T(" old messages"));
        backup();
        accept();
        udpRead();
        tick();
        logf(std::string(T("Eco ")) + kVersion + T(" listening on port ") + std::to_string(cfg_.port) + T(" (TCP+UDP), data in ") + cfg_.dbFile);
        logf(T("Server fingerprint: ") + fingerprint(idPub_));
        printInvites();
    }

    // Invitation ready to paste into Eco: address, port, fingerprint (the first connection is verified)
    // and, if there is one, the server password. The local IP comes from the default route: "connecting" a UDP
    // socket sends nothing to the network, it only asks the system which interface it would go out through.
    void printInvites() {
        std::string ip;
        try {
            asio::io_context tmp;
            udp::socket probe(tmp, udp::v4());
            probe.connect(udp::endpoint(asio::ip::make_address_v4("192.0.2.1"), 9));  // TEST-NET: never used
            ip = probe.local_endpoint().address().to_string();
        } catch (const std::exception&) {
        }
        auto inv = [&](const std::string& host) { return makeInvite(host, cfg_.port, fingerprint(idPub_), cfg_.serverPass); };
        std::cout << T("\nInvitation for your friends (they paste it into Eco, in the \"Server or invitation\" field):\n");
        bool priv = ip.rfind("10.", 0) == 0 || ip.rfind("192.168.", 0) == 0 || ip.rfind("127.", 0) == 0 ||
                    ip.rfind("169.254.", 0) == 0 ||
                    (ip.rfind("100.", 0) == 0 && std::atoi(ip.c_str() + 4) >= 64 && std::atoi(ip.c_str() + 4) <= 127) ||  // CGNAT
                    (ip.rfind("172.", 0) == 0 && ip.size() > 6 && std::atoi(ip.c_str() + 4) >= 16 && std::atoi(ip.c_str() + 4) <= 31);
        if (!ip.empty() && !priv) {
            std::cout << "  " << inv(ip) << "\n";
        } else {
            if (!ip.empty()) std::cout << T("  On your own network (home, office): ") << inv(ip) << "\n";
            std::cout << T("  Over the Internet: ") << inv("TU-IP-PUBLICA") << "\n"
                      << T("    (replace TU-IP-PUBLICA with your public IP or your domain and open in the router port ")
                      << cfg_.port << T(" TCP and UDP)\n");
        }
        if (!cfg_.serverPass.empty()) std::cout << T("  It contains the server's password: only give it to your friends.\n");
        std::cout << T("  Anyone already inside can also copy it from Eco: Eco menu > Invite a friend.\n\n")
                  << std::flush;
    }

    void onMessage(const SessionPtr& s, Msg type, Reader& r);
    void onDisconnect(const SessionPtr& s);
    void onSecure(const SessionPtr& s) {
        if (isBanned(s)) kick(s, "Estás baneado de este servidor");
    }
    bool isBanned(const SessionPtr& s) const { return bans_.count(s->ip) || bans_.count(s->ipGroup); }

    const uint8_t* idSecret() const { return idSecret_; }
    const uint8_t* idPublic() const { return idPub_; }

private:
    void loadIdentity() {
        // The server identity (Ed25519) is created once and lives in the database
        auto seed = db_.blob("identity_seed");
        if (seed.size() != 32) {
            seed.resize(32);
            secureRandom(seed.data(), 32);
            db_.setBlob("identity_seed", seed);
        }
        crypto_eddsa_key_pair(idSecret_, idPub_, seed.data());  // wipes the seed from memory
    }

    void accept() {
        acceptor_.async_accept([this](std::error_code ec, tcp::socket sock) {
            if (ec) {
                // E.g. the system ran out of descriptors: retrying instantly would be a loop that
                // burns the CPU (and cuts the voice). Wait a moment.
                if (ec == asio::error::operation_aborted) return;
                acceptRetry_.expires_after(std::chrono::milliseconds(200));
                acceptRetry_.async_wait([this](std::error_code e) { if (!e) accept(); });
                return;
            }
            std::error_code e2;
            sock.set_option(tcp::no_delay(true), e2);
            auto s = std::make_shared<Session>(std::move(sock), *this);
            auto addr = s->sock.remote_endpoint(e2).address();
            if (addr.is_v6() && addr.to_v6().is_v4_mapped())
                addr = asio::ip::make_address_v4(asio::ip::v4_mapped, addr.to_v6());
            s->ip = addr.to_string();
            s->ipGroup = ipKey(s->ip);
            size_t same = 0;
            for (auto& [id, o] : sessions_) same += o->ipGroup == s->ipGroup;
            // Too many at once from the same IP, or too close together: cut before the key exchange
            if (e2 || same >= kMaxConnsPerIp || !allowConnection(s->ipGroup)) s->shutdownSocket();
            else if (makeRoom()) s->start();
            else s->shutdownSocket();
            accept();
        });
    }

    bool allowConnection(const std::string& group) {
        auto now = std::chrono::steady_clock::now();
        auto [it, fresh] = connRate_.try_emplace(group);
        ConnRate& r = it->second;
        if (fresh) r.tokens = kConnBurst;
        else r.tokens = std::min(kConnBurst, r.tokens + std::chrono::duration<double>(now - r.at).count() * kConnPerSec);
        r.at = now;
        if (r.tokens < 1) return false;
        r.tokens -= 1;
        return true;
    }

    // Global cap on open connections (each one takes a system descriptor). If it is full, the oldest
    // connection that hasn't identified yet gives up its place.
    bool makeRoom() {
        if (sessions_.size() < cfg_.maxUsers + kMaxPendingConns) return true;
        for (auto& [id, o] : sessions_)  // sorted by id: the first one is the oldest
            if (!o->authed && !o->authBusy) {
                SessionPtr victim = o;
                victim->close();
                return true;
            }
        return false;
    }

    template <class F>
    static std::vector<uint8_t> w(F f) { Writer x; f(x); return std::move(x.buf); }

    // What the server tells clients travels in Spanish, as in every 2.x version: each client translates it
    // (i18n::fromServer), so clients and servers of different 2.x versions keep understanding each other.
    void info(const SessionPtr& s, const std::string& t) { s->send(Msg::Info, w([&](Writer& x) { x.str(t); })); }
    void error(const SessionPtr& s, const std::string& t) { s->send(Msg::Error, w([&](Writer& x) { x.str(t); })); }

    void infoChannel(const std::string& ch, const std::string& t) {
        for (auto& [id, s] : sessions_)
            if (s->authed && s->channel == ch) info(s, t);
    }

    // Case-insensitive comparison without making copies
    static bool iequals(const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
               });
    }

    SessionPtr byNick(const std::string& n) {
        for (auto& [id, s] : sessions_)
            if (s->authed && iequals(s->nick, n)) return s;
        return nullptr;
    }

    bool channelExists(const std::string& n, std::string* real = nullptr) {
        for (auto& c : channels_)
            if (iequals(c, n)) { if (real) *real = c; return true; }
        return false;
    }

    void broadcastState() {
        Writer x;
        x.u16(uint16_t(channels_.size()));
        for (auto& ch : channels_) {
            x.str(ch);
            std::vector<SessionPtr> users;
            for (auto& [id, s] : sessions_)
                if (s->authed && s->channel == ch) users.push_back(s);
            x.u16(uint16_t(users.size()));
            for (auto& u : users) {
                x.u32(u->id);
                x.str(u->nick);
                x.u8(uint8_t(u->role));
                x.u8(uint8_t((u->sharing ? kUserSharing : 0) | (u->serverMuted ? kUserMuted : 0) |
                             (u->registered ? kUserRegistered : 0) | (u->sharing && u->sharingAudio ? kUserScreenAudio : 0)));
                x.raw(u->pubkey, 32);
            }
        }
        auto plain = body(Msg::State, x.buf);
        for (auto& [id, s] : sessions_)
            if (s->authed) s->sendBody(plain);
    }

    void sendHistory(const SessionPtr& s, const std::string& title, const std::string& channel,
                     const std::vector<Db::Message>& msgs) {
        Writer x;
        x.str(title);
        x.str(channel);
        x.u16(uint16_t(msgs.size()));
        for (auto& m : msgs) { x.u64(uint64_t(m.ts)); x.str(m.from); x.str(m.to); x.str(m.text); }
        s->send(Msg::History, x.buf);
    }

    void sendChannelHistory(const SessionPtr& s, int n) {
        auto msgs = db_.history(s->channel, n);
        if (!msgs.empty()) sendHistory(s, "Últimos mensajes de " + s->channel, s->channel, msgs);
    }

    void moveTo(const SessionPtr& s, const std::string& ch) {
        if (s->channel == ch) return;
        std::string old = s->channel;
        stopSharing(s, false);
        uint32_t was = s->watching;
        s->watching = 0;
        s->channel = ch;
        if (was) notifyViewers(was);
        if (!old.empty()) infoChannel(old, s->nick + " salió del canal");
        infoChannel(ch, s->nick + " entró a " + ch);
        broadcastState();
        sendChannelHistory(s, cfg_.joinHistory);
    }

    void stopSharing(const SessionPtr& s, bool announce) {
        if (!s->sharing) return;
        s->sharing = false;
        s->sharingAudio = false;
        for (auto& [id, o] : sessions_)
            if (o->watching == s->id) { o->watching = 0; info(o, s->nick + " dejó de compartir pantalla"); }
        if (announce) broadcastState();
    }

    // Tells whoever shares how many are watching (if nobody watches, nothing is sent)
    void notifyViewers(uint32_t sharerId) {
        auto it = sessions_.find(sharerId);
        if (it == sessions_.end() || !it->second->sharing) return;
        uint16_t n = 0;
        for (auto& [id, o] : sessions_)
            if (o->watching == sharerId && o->channel == it->second->channel) ++n;
        it->second->send(Msg::Viewers, w([&](Writer& x) { x.u16(n); }));
    }

    void kick(const SessionPtr& target, const std::string& why) {
        target->send(Msg::Kicked, w([&](Writer& x) { x.str(why); }));
        target->close();
    }

    // ---- Brake on password guessing attempts (per IP or /64, and per account) ----
    struct Fails { int count = 0; std::chrono::steady_clock::time_point first; };
    bool tooManyFails(const SessionPtr& s) {
        auto it = fails_.find(s->ipGroup);
        return it != fails_.end() && it->second.count >= kIpFailLimit;
    }
    void noteFail(const SessionPtr& s) {
        auto& f = fails_[s->ipGroup];
        if (f.count++ == 0) f.first = std::chrono::steady_clock::now();
    }
    // Per account: slows down whoever tries from many different IPs. It only delays (15 min), it doesn't block forever
    bool accountLocked(const std::string& nick) {
        auto it = acctFails_.find(lower(nick));
        return it != acctFails_.end() && it->second.count >= kAcctFailLimit;
    }
    void noteAccountFail(const std::string& nick) {
        auto& f = acctFails_[lower(nick)];
        if (f.count++ == 0) f.first = std::chrono::steady_clock::now();
    }

    void handleHello(const SessionPtr& s, Reader& r);
    void handleCommand(const SessionPtr& s, const std::string& line);

    // ---- UDP: voice relay ----
    void udpRead() {
        udp_.async_receive_from(asio::buffer(udpBuf_), udpFrom_, [this](std::error_code ec, size_t n) {
            if (!ec) handleUdp(n);
            udpRead();
        });
    }

    // Direct send with the socket in non-blocking mode: UDP almost never has to wait, and if the
    // system buffer were full the packet is dropped (for voice that is better than arriving late).
    // This way no memory is allocated and no handler is queued for each packet and listener.
    void udpSendTo(const SessionPtr& o, const uint8_t* plain, size_t n) {
        udpSealInto(udpSeal_, o->keys.udpS2C, o->udpTx++, nullptr, 0, plain, n);
        std::error_code ec;
        udp_.send_to(asio::buffer(udpSeal_), o->udpEp, 0, ec);
    }


    void handleUdp(size_t n) {
        if (n < 4 + 8 + 1 + kMacSize) return;
        const uint8_t* p = udpBuf_.data();
        uint32_t id = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        auto it = sessions_.find(id);
        if (it == sessions_.end() || !it->second->authed) return;
        auto& s = it->second;
        uint64_t ctr;
        // Only whoever has the session keys can produce a valid packet
        if (!udpOpen(s->keys.udpC2S, p, n, 4, ctr, udpPlain_) || !s->udpRx.accept(ctr)) return;
        s->udpEp = udpFrom_;
        s->hasUdp = true;
        s->lastSeen = std::chrono::steady_clock::now();
        // After authenticating (otherwise anyone would spend someone else's tokens by forging their id)
        if (!s->allowUdp()) return;
        if (udpPlain_[0] == kUdpPing) {
            uint8_t pong = kUdpPing;
            udpSendTo(s, &pong, 1);
            return;
        }
        relay(s, udpPlain_);
    }

    // Relays voice or screen audio. "in" is the plaintext packet as the client sends it, whether it arrives over
    // UDP or, if their UDP is blocked, over TCP (VoiceTcp). Each listener receives it through whatever works for them.
    void relay(const SessionPtr& s, const std::vector<uint8_t>& in) {
        if (in.size() < 2 || in.size() > kMaxUdpPayload || s->serverMuted) return;
        uint8_t kind = in[0];
        bool screen = kind == kUdpScreenAudio;
        if (screen) {
            if (!s->sharing || !(in[1] & kScreenAudio)) return;
            s->lastScreenAudio = std::chrono::steady_clock::now();
            if (!s->sharingAudio) { s->sharingAudio = true; broadcastState(); }  // the others see that it carries audio
        } else if ((kind != kUdpVoice && kind != kUdpVoiceE2E && kind != kUdpVoiceAuto) || in.size() < 4) {
            return;
        }
        // type, sourceId and the rest as is (with E2E it is encrypted and can't be read here)
        udpOut_.clear();
        udpOut_.push_back(kind);
        for (int sh = 24; sh >= 0; sh -= 8) udpOut_.push_back(uint8_t(s->id >> sh));
        udpOut_.insert(udpOut_.end(), in.begin() + 1, in.end());
        tcpOut_.clear();
        for (auto& [oid, o] : sessions_) {
            if (oid == s->id || !o->authed || o->channel != s->channel) continue;
            if (screen && o->watching != s->id) continue;  // the screen audio, only to whoever is watching it
            if (o->tcpVoice) {
                if (o->queuedBytes() > 64 * 1024) continue;  // falling behind: better to lose voice than to pile up delay
                if (tcpOut_.empty()) {
                    tcpOut_.push_back(uint8_t(Msg::VoiceFrom));
                    tcpOut_.insert(tcpOut_.end(), udpOut_.begin(), udpOut_.end());
                }
                o->sendBody(tcpOut_);
            } else if (o->hasUdp) {
                udpSendTo(o, udpOut_.data(), udpOut_.size());
            }
        }
    }

    // ---- Backups of eco.db (one a day, the last cfg_.backupKeep are kept) ----
    void backup() {
        if (cfg_.backupKeep <= 0) return;
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dir = fs::path(cfg_.dbFile).parent_path() / "copias";
        fs::create_directories(dir, ec);
        auto t = std::time(nullptr);
        char day[16];
        std::strftime(day, sizeof day, "%Y-%m-%d", std::localtime(&t));
        fs::path out = dir / (std::string("eco-") + day + ".db");
        if (fs::exists(out, ec)) return;  // today's is already there
        std::string err;
        if (!db_.backupTo(out.string(), err)) return logf(T("Could not make the backup: ") + err);
#ifndef _WIN32
        ::chmod(dir.string().c_str(), 0700);  // it carries the server identity: only for its user
        ::chmod(out.string().c_str(), 0600);
#endif
        logf(T("Database backup: ") + out.string());
        std::vector<fs::path> all;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            std::string n = e.path().filename().string();
            if (n.rfind("eco-", 0) == 0 && n.size() > 7 && n.compare(n.size() - 3, 3, ".db") == 0) all.push_back(e.path());
        }
        std::sort(all.begin(), all.end());  // by date (it's in the name)
        while (all.size() > size_t(cfg_.backupKeep)) { fs::remove(all.front(), ec); all.erase(all.begin()); }
    }

    // ---- Updates: the server only hands out files; the client checks the signature ----
    static bool safeName(const std::string& n, size_t max) {
        if (n.empty() || n.size() > max || n[0] == '.') return false;
        for (char c : n)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') return false;
        // Windows device names ("CON", "nul.txt", "COM1"...): opening them doesn't read a file, and "CON"
        // would leave the server waiting for the console
        std::string base = lower(n.substr(0, n.find('.')));
        static const char* reserved[] = {"con", "prn", "aux", "nul"};
        for (auto* r : reserved) if (base == r) return false;
        if (base.size() == 4 && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) && std::isdigit((unsigned char)base[3]))
            return false;
        return true;
    }
    static bool readFile(const std::filesystem::path& p, std::vector<uint8_t>& out, size_t max) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return out.size() <= max;
    }
    // manifest.*: since 2.3; manifiesto.*: what the signing tool published before
    static std::string manifestBase(const std::filesystem::path& dir) {
        std::error_code ec;
        return std::filesystem::exists(dir / "manifest.txt", ec) ? "manifest" : "manifiesto";
    }
    void handleUpdateQuery(const SessionPtr& s, const std::string& platform) {
        if (cfg_.updateDir.empty() || !safeName(platform, 32)) return;
        auto dir = std::filesystem::path(cfg_.updateDir) / platform;
        std::vector<uint8_t> man, sig;
        std::string base = manifestBase(dir);
        if (!readFile(dir / (base + ".txt"), man, 64 * 1024) || !readFile(dir / (base + ".sig"), sig, 64) || sig.size() != 64)
            return;
        s->send(Msg::UpdateInfo, w([&](Writer& x) {
            x.str(platform);
            x.bytes(man.data(), man.size());
            x.bytes(sig.data(), sig.size());
        }));
    }
    void handleUpdateGet(const SessionPtr& s, const std::string& platform, const std::string& file, uint32_t from) {
        if (cfg_.updateDir.empty() || !safeName(platform, 32) || !safeName(file, 64) || s->queuedBytes() > 2u * 1024 * 1024)
            return;
        auto dir = std::filesystem::path(cfg_.updateDir) / platform;
        // Only the files named by the published manifest are served, nothing else from the folder
        std::vector<uint8_t> man;
        if (!readFile(dir / (manifestBase(dir) + ".txt"), man, 64 * 1024)) return;
        std::string text(man.begin(), man.end()), want = "\narchivo " + file + " ";
        if (text.find(want) == std::string::npos) return error(s, "Ese archivo no forma parte de la actualización");
        auto p = dir / file;
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) return error(s, "Esa actualización ya no está en el servidor");
        auto total = uint64_t(f.tellg());
        if (total > 128u * 1024 * 1024 || from >= total) return;
        std::vector<uint8_t> chunk(size_t(std::min<uint64_t>(256 * 1024, total - from)));
        f.seekg(std::streamoff(from));
        f.read(reinterpret_cast<char*>(chunk.data()), std::streamsize(chunk.size()));
        if (!f) return;
        s->send(Msg::UpdateChunk, w([&](Writer& x) {
            x.str(file);
            x.u32(from);
            x.u32(uint32_t(total));
            x.bytes(chunk.data(), chunk.size());
        }));
    }

    void tick() {
        timer_.expires_after(std::chrono::seconds(10));
        timer_.async_wait([this](std::error_code ec) {
            if (ec) return;
            auto now = std::chrono::steady_clock::now();
            std::vector<SessionPtr> dead;
            for (auto& [id, s] : sessions_)
                if ((s->authed && now - s->lastSeen > std::chrono::seconds(45)) ||
                    (!s->authed && !s->authBusy && now > s->authDeadline))
                    dead.push_back(s);  // idle, or connected without identifying (takes a slot)
            for (auto& s : dead) {
                if (s->authed) g_noisy.line(T("Timed out: ") + s->nick);
                s->close();
            }
            for (auto it = fails_.begin(); it != fails_.end();)
                it = now - it->second.first > std::chrono::minutes(5) ? fails_.erase(it) : std::next(it);
            for (auto it = acctFails_.begin(); it != acctFails_.end();)
                it = now - it->second.first > std::chrono::minutes(15) ? acctFails_.erase(it) : std::next(it);
            for (auto it = connRate_.begin(); it != connRate_.end();)  // already recovered all its tokens
                it = now - it->second.at > std::chrono::seconds(60) ? connRate_.erase(it) : std::next(it);
            if (++ticks_ % 8640 == 0) db_.prune(cfg_.retainDays);  // once a day
            if (ticks_ % 360 == 0) backup();                       // every hour, check whether today's copy already exists
            g_noisy.roll();
            bool changed = false;  // did someone stop sending their screen audio?
            for (auto& [id, o] : sessions_)
                if (o->sharingAudio && now - o->lastScreenAudio > std::chrono::seconds(5)) { o->sharingAudio = false; changed = true; }
            if (changed) broadcastState();
            tick();
        });
    }

    // Argon2id on a separate thread: the main thread keeps relaying voice while someone logs in.
    // done() runs back on the main thread.
    // Limits: 16 at a time in total and 2 per IP (or /64), so nobody hogs the queue and keeps the others from logging in
    template <class F>
    bool hashAsync(const std::string& group, std::string pass, std::vector<uint8_t> salt, int64_t kib, int64_t passes,
                   F done) {
        if (hashJobs_ >= kMaxHashJobs || hashByIp_[group] >= kMaxHashJobsPerIp) {
            if (!hashByIp_[group]) hashByIp_.erase(group);
            return false;
        }
        ++hashJobs_;
        ++hashByIp_[group];
        asio::post(hashPool_, [this, group, pass = std::move(pass), salt = std::move(salt), kib, passes,
                               done = std::move(done)]() mutable {
            auto h = Db::hashPassword(pass, salt, kib, passes);
            if (!pass.empty()) crypto_wipe(&pass[0], pass.size());
            asio::post(io_, [this, group, h = std::move(h), done = std::move(done)]() mutable {
                --hashJobs_;
                if (--hashByIp_[group] <= 0) hashByIp_.erase(group);
                done(h);
            });
        });
        return true;
    }
    bool canActOnAdmin(const SessionPtr& s) {
        if (s->owner) return true;
        error(s, "Solo quien entró con la clave de admin del servidor puede hacer eso a otro admin");
        return false;
    }
    void finishLogin(const SessionPtr& s, const std::string& nick, Role role, bool registered);

    asio::io_context& io_;
    Config cfg_;
    Db& db_;
    tcp::acceptor acceptor_;
    udp::socket udp_;
    asio::steady_timer timer_, acceptRetry_;
    std::array<uint8_t, 2048> udpBuf_{};
    std::vector<uint8_t> udpPlain_, udpOut_, udpSeal_, tcpOut_;
    udp::endpoint udpFrom_;
    std::map<uint32_t, SessionPtr> sessions_;
    std::vector<std::string> channels_;
    std::set<std::string> bans_;
    std::set<std::string> banAccounts_, muteAccounts_, muteIps_;  // lowercase (IPs as is)
    std::map<std::string, Fails> fails_, acctFails_;
    struct ConnRate { double tokens = 0; std::chrono::steady_clock::time_point at; };
    std::map<std::string, ConnRate> connRate_;  // brake on new connections per IP (or /64)
    std::map<std::string, int> hashByIp_;
    bool masterKey_ = false;  // cfg_.adminPass is the temporary master password (deleted when used)
    uint8_t idSecret_[64], idPub_[32];
    uint32_t nextId_ = 1;
    uint64_t ticks_ = 0;
    int hashJobs_ = 0;
    asio::thread_pool hashPool_{1};  // the last one: destroyed (and waits for its thread) before everything else

    friend class Session;
    void add(const SessionPtr& s) { s->id = nextId_++; sessions_[s->id] = s; }
};

// ---------------- Session ----------------
void Session::start() {
    server.add(shared_from_this());
    readHeader();
}

void Session::readHeader() {
    auto self = shared_from_this();
    asio::async_read(sock, asio::buffer(hdr_), [this, self](std::error_code ec, size_t) {
        if (ec) return close();
        uint32_t len = uint32_t(hdr_[0]) << 24 | uint32_t(hdr_[1]) << 16 | uint32_t(hdr_[2]) << 8 | hdr_[3];
        if (len == 0 || len > kMaxFrame || (!secure && len != kClientHelloSize) || (!authed && len > kMaxPreAuthFrame))
            return close();
        readBody(len);
    });
}

// Key exchange: validates the client greeting and replies signed
bool Session::handshake() {
    Reader r(body_.data(), body_.size());
    if (std::memcmp(r.ptr(), "ECO2", 4) != 0) return false;
    r.skip(4);
    uint32_t version = r.u32();
    const uint8_t* cpub = r.ptr();

    uint8_t esk[32], spub[32], shared[32];
    secureRandom(esk, 32);
    crypto_x25519_public_key(spub, esk);
    crypto_x25519(shared, esk, cpub);
    static const uint8_t zero[32] = {0};
    if (crypto_verify32(shared, zero) == 0) {  // invalid public key (low order)
        crypto_wipe(esk, 32);
        return false;
    }
    deriveKeys(shared, cpub, spub, server.idPublic(), keys);
    crypto_wipe(esk, 32);
    crypto_wipe(shared, 32);

    auto transcript = handshakeTranscript(version, cpub, spub);
    auto reply = std::make_shared<std::vector<uint8_t>>(4 + kServerHelloSize);
    uint8_t* o = reply->data();
    o[0] = o[1] = o[2] = 0;
    o[3] = uint8_t(kServerHelloSize);
    std::memcpy(o + 4, spub, 32);
    std::memcpy(o + 36, server.idPublic(), 32);
    crypto_eddsa_sign(o + 68, server.idSecret(), transcript.data(), transcript.size());
    sendWire(reply);

    tx.key = keys.tcpS2C;
    rx.key = keys.tcpC2S;
    secure = true;
    if (version != kProtoVersion) {
        send(Msg::Kicked, [] { Writer x; x.str("Versión incompatible: actualiza Eco en el cliente y el servidor"); return x.buf; }());
        close();
        return true;
    }
    server.onSecure(shared_from_this());
    return true;
}

void Session::readBody(uint32_t len) {
    body_.resize(len);
    auto self = shared_from_this();
    asio::async_read(sock, asio::buffer(body_), [this, self](std::error_code ec, size_t) {
        if (ec) return close();
        lastSeen = std::chrono::steady_clock::now();
        try {
            if (!secure) {
                if (!handshake()) return close();
            } else {
                // An authentication failure means tampered data: the connection is cut
                if (!rx.open(body_.data(), body_.size())) return close();
                Reader r(body_.data() + 1, body_.size() - kMacSize - 1);
                server.onMessage(self, Msg(body_[0]), r);
            }
        } catch (const std::exception&) {
            return close();
        }
        // Whoever no longer shares their screen doesn't need a buffer of megabytes: it goes back to the system
        if (!sharing && body_.capacity() > kKeepReadBuffer) std::vector<uint8_t>().swap(body_);
        if (!closed_) readHeader();
    });
}

void Session::sendWire(std::shared_ptr<std::vector<uint8_t>> data) {
    if (closed_) return;
    if (outBytes_ + data->size() > kMaxQueuedBytes) {
        // Doesn't read what is sent to it: it gets disconnected (afterwards, so as not to touch sessions_ while
        // the caller is iterating over it)
        if (!overflow_) {
            overflow_ = true;
            asio::post(sock.get_executor(), [self = shared_from_this()] { self->close(); });
        }
        return;
    }
    outBytes_ += data->size();
    outq_.push_back(std::move(data));
    if (outq_.size() == 1) doWrite();
}

void Session::doWrite() {
    auto self = shared_from_this();
    asio::async_write(sock, asio::buffer(*outq_.front()), [this, self](std::error_code ec, size_t) {
        if (ec) { outq_.clear(); shutdownSocket(); return close(); }
        outBytes_ -= outq_.front()->size();
        outq_.pop_front();
        if (!outq_.empty()) doWrite();
        else if (closed_) shutdownSocket();
    });
}

void Session::shutdownSocket() {
    std::error_code ec;
    sock.shutdown(tcp::socket::shutdown_both, ec);
    sock.close(ec);
}

void Session::close() {
    if (closed_) return;
    closed_ = true;
    // If there are messages left (e.g. "Kicked"), it closes after sending them
    if (outq_.empty()) shutdownSocket();
    else {
        // Security: don't wait forever for a client that doesn't read
        auto self = shared_from_this();
        auto t = std::make_shared<asio::steady_timer>(sock.get_executor(), std::chrono::seconds(3));
        t->async_wait([self, t](std::error_code) { self->shutdownSocket(); });
    }
    server.onDisconnect(shared_from_this());
}

// ---------------- Logic ----------------
void Server::onDisconnect(const SessionPtr& s) {
    if (!sessions_.erase(s->id)) return;
    if (s->authed) {
        stopSharing(s, false);
        if (s->watching) notifyViewers(s->watching);
        g_noisy.line(s->nick + T(" disconnected"));
        infoChannel(s->channel, s->nick + " se desconectó");
        broadcastState();
    }
}

void Server::handleHello(const SessionPtr& s, Reader& r) {
    if (s->authed || s->authBusy) return;
    std::string nick = trim(r.str()), pass = r.str(), spass = r.str();
    if (r.left() < 32) return kick(s, "Versión de cliente incompatible");
    std::memcpy(s->pubkey, r.ptr(), 32);
    r.skip(32);
    s->clientVersion = r.left() >= 2 ? cleanVersion(r.str()) : "";
    if (tooManyFails(s)) return kick(s, "Demasiados intentos fallidos. Espera unos minutos.");
    if (!cfg_.serverPass.empty() && !secretEquals(spass, cfg_.serverPass)) {
        noteFail(s);
        return kick(s, "Contraseña del servidor incorrecta");
    }
    if (!validNick(nick))
        return kick(s, "Nick inválido (1-24 caracteres, sin espacios, sin caracteres invisibles ni mezclar alfabetos)");

    if (auto acct = db_.account(nick)) {
        auto fail = [this, s](const std::string& t) {
            // Time to type it, but with an overall cap: resending Hello doesn't extend the wait endlessly
            s->authDeadline = std::min(std::chrono::steady_clock::now() + std::chrono::minutes(2),
                                       s->connectedAt + std::chrono::minutes(5));
            s->send(Msg::AuthFail, w([&](Writer& x) { x.str(t); }));
        };
        if (pass.empty()) return fail("El nick '" + acct->nick + "' está registrado. Escribe su contraseña:");
        if (accountLocked(acct->nick))
            return kick(s, "Demasiados intentos fallidos con esta cuenta. Espera unos minutos.");
        auto cred = db_.credentials(nick);
        if (!cred) return kick(s, "No se pudo leer la cuenta");
        s->authBusy = true;
        bool queued = hashAsync(s->ipGroup, std::move(pass), cred->salt, cred->kib, cred->passes,
                                [this, s, fail, acct = *acct, stored = cred->hash](const std::vector<uint8_t>& got) {
                                    s->authBusy = false;
                                    if (s->isClosed() || s->authed) return;
                                    if (got.size() != 32 || stored.size() != 32 ||
                                        crypto_verify32(got.data(), stored.data()) != 0) {
                                        noteFail(s);
                                        noteAccountFail(acct.nick);
                                        g_noisy.line(T("Wrong password for ") + acct.nick + T(" from ") + s->ip);
                                        if (++s->authFails >= 3) return kick(s, "Contraseña incorrecta");
                                        return fail("Contraseña incorrecta. Inténtalo de nuevo:");
                                    }
                                    finishLogin(s, acct.nick, acct.role, true);
                                });
        if (!queued) {
            s->authBusy = false;
            kick(s, "El servidor está ocupado: vuelve a intentarlo en unos segundos");
        }
        return;
    }
    if (byNick(nick)) return kick(s, "Ese nick ya está en uso");
    finishLogin(s, nick, Role::User, false);
}

void Server::finishLogin(const SessionPtr& s, const std::string& nick, Role role, bool registered) {
    if (registered) {
        if (auto old = byNick(nick); old && old != s) kick(old, "Iniciaste sesión desde otro lugar");
        db_.touchLogin(nick);
        db_.setPubkey(nick, std::vector<uint8_t>(s->pubkey, s->pubkey + 32));
    }
    if (registered && banAccounts_.count(lower(nick))) return kick(s, "Esta cuenta está baneada de este servidor");
    size_t n = 0;
    for (auto& [i, o] : sessions_) n += o->authed;
    if (n >= cfg_.maxUsers) return kick(s, "Servidor lleno");
    // A mute persists after reconnecting: by account if they have one; if guest, by their IP (or /64)
    s->serverMuted = registered ? muteAccounts_.count(lower(nick)) > 0 : muteIps_.count(s->ipGroup) > 0;

    s->nick = nick;
    s->role = role;
    s->registered = registered;
    s->authed = true;
    s->channel = channels_.front();
    s->send(Msg::Welcome, w([&](Writer& x) {
        x.u32(s->id); x.u16(cfg_.port); x.u8(uint8_t(s->role)); x.u8(registered ? 1 : 0);
        x.u8(uint8_t(kFeatScreenAudio | kFeatTcpVoice | kFeatPong | (cfg_.updateDir.empty() ? 0 : kFeatUpdates)));
        x.str(kVersion);
    }));
    g_noisy.line(nick + T(" connected from ") + s->ip + " (" + T(roleLabel(s->role)) + (registered ? T(", account") : T(", guest")) +
         T(", Eco ") + (s->clientVersion.empty() ? std::string(T("older than 2.1")) : s->clientVersion) + ")");
    infoChannel(s->channel, nick + " se conectó");
    broadcastState();
    sendChannelHistory(s, cfg_.joinHistory);
    if (registered) {
        // In batches of 200: only the ones really sent are marked as delivered
        for (int batch = 0; batch < 10; ++batch) {
            auto pending = db_.takePending(nick, 200);
            if (pending.empty()) break;
            sendHistory(s, "Mensajes privados que te dejaron mientras no estabas", "", pending);
        }
    }
}

void Server::onMessage(const SessionPtr& s, Msg type, Reader& r) {
    if (!s->authed && type != Msg::Hello) return s->close();

    // Frames and pings don't count (they have their own control); the rest, including the greeting, goes through the
    // token bucket (before identifying, if the tokens run out the connection is cut: nobody legitimate greets 40 times)
    // Voice over TCP has the same brake as over UDP
    // Empty (keepalive) Pings don't either; the ones that ask for a Pong do count
    bool exempt = type == Msg::ScreenFrame || type == Msg::VoiceTcp || (type == Msg::Ping && r.left() == 0);
    if (!exempt && !s->allowMessage()) {
        if (!s->authed) return s->close();
        auto now = std::chrono::steady_clock::now();
        if (now - s->floodWarned > std::chrono::seconds(2)) {
            s->floodWarned = now;
            error(s, "Vas demasiado rápido: espera un momento");
        }
        return;
    }
    switch (type) {
        case Msg::Hello: handleHello(s, r); break;
        case Msg::Join: {
            std::string ch = trim(r.str()), real;
            if (!channelExists(ch, &real)) return error(s, "No existe el canal '" + ch + "'");
            moveTo(s, real);
            break;
        }
        case Msg::Chat: {
            std::string text = r.str();
            if (trim(text).empty()) return;
            // Every end-to-end encrypted format starts with byte 1: plaintext is not accepted
            if (text[0] != kSealedPrefix) return error(s, "Solo se aceptan mensajes cifrados: actualiza Eco");
            if (text.size() > kMaxSealedText) return error(s, "Mensaje demasiado largo");
            db_.addChannelMessage(s->channel, s->nick, text);
            auto plain = body(Msg::ChatMsg, w([&](Writer& x) { x.str(s->channel); x.str(s->nick); x.str(text); x.u8(0); }));
            for (auto& [id, o] : sessions_)
                if (o->authed && o->channel == s->channel) o->sendBody(plain);
            break;
        }
        case Msg::Command: handleCommand(s, r.str()); break;
        case Msg::Ping:
            if (r.left() >= 8) s->send(Msg::Pong, w([&](Writer& x) { x.u64(r.u64()); }));  // to measure the ping
            break;
        case Msg::VoiceTcp: {
            if (!s->allowUdp()) return;
            std::vector<uint8_t> in(r.ptr(), r.ptr() + r.left());
            relay(s, in);
            break;
        }
        case Msg::TcpVoiceMode: s->tcpVoice = r.left() >= 1 && r.u8() != 0; break;
        case Msg::UpdateQuery: handleUpdateQuery(s, r.str()); break;
        case Msg::UpdateGet: {
            std::string platform = r.str(), file = r.str();
            handleUpdateGet(s, platform, file, r.u32());
            break;
        }
        case Msg::KeyShare: {
            // Encrypted sender keys from one member for others in the same channel: they are only relayed
            int n = std::min<int>(r.u16(), 256);
            for (int i = 0; i < n; ++i) {
                uint32_t target = r.u32();
                auto blob = r.bytes();
                auto it = sessions_.find(target);
                if (blob.size() > 256 || it == sessions_.end() || !it->second->authed || it->second->channel != s->channel)
                    continue;
                it->second->send(Msg::KeyFrom, w([&](Writer& x) { x.u32(s->id); x.bytes(blob.data(), blob.size()); }));
            }
            break;
        }
        case Msg::ScreenStart:
            if (!s->sharing) {
                s->sharing = true;
                infoChannel(s->channel, s->nick + " está compartiendo pantalla. Usa /ver " + s->nick);
                broadcastState();
            }
            break;
        case Msg::ScreenStop: stopSharing(s, true); break;
        case Msg::ScreenFrame: {
            if (!s->sharing) return;
            bool isKey = r.left() >= 1 && (r.ptr()[0] & kScreenKey);
            // Byte brake: a stream of huge frames must not hog the only thread (voice)
            if (!s->allowScreen(std::max(r.left(), kScreenMinCost))) {
                // Dropped; whoever watches needs a key frame not to be left halfway
                for (auto& [id, o] : sessions_)
                    if (o->watching == s->id) o->needKey = true;
                return;
            }
            std::vector<uint8_t> plain;
            bool wantKey = false;
            for (auto& [id, o] : sessions_) {
                if (o->watching != s->id || o->channel != s->channel) continue;
                if (o->needKey && !isKey) { wantKey = true; continue; }
                // Congestion control: if the viewer is behind, frames are skipped
                if (o->queuedBytes() > 2u * 1024u * 1024u) { o->needKey = true; wantKey = true; continue; }
                if (plain.empty()) {
                    Writer x;
                    x.u8(uint8_t(Msg::ScreenFrom));
                    x.u32(s->id);
                    x.raw(r.ptr(), r.left());
                    plain = std::move(x.buf);
                }
                o->needKey = false;
                o->sendBody(plain);
            }
            if (wantKey) s->send(Msg::KeyRequest);
            break;
        }
        case Msg::Watch: {
            uint32_t target = r.u32();
            uint32_t was = s->watching;
            s->watching = 0;
            if (was) notifyViewers(was);
            if (target == 0) return;
            auto it = sessions_.find(target);
            if (it == sessions_.end() || !it->second->sharing || it->second->channel != s->channel)
                return error(s, "Ese usuario no está compartiendo pantalla en tu canal");
            s->watching = target;
            s->needKey = true;
            it->second->send(Msg::KeyRequest);
            notifyViewers(target);
            break;
        }
        default: break;
    }
}

void Server::handleCommand(const SessionPtr& s, const std::string& line) {
    auto a = split(line);
    if (a.empty()) return;
    std::string cmd = lower(a[0]);
    auto rest = [&](size_t from) {
        std::string out;
        for (size_t i = from; i < a.size(); ++i) out += (i > from ? " " : "") + a[i];
        return out;
    };
    // The rest of the line as is, after "from" words: passwords can contain spaces (even
    // double or at the end) and must be saved exactly as they are typed later when logging in
    auto rawRest = [&](size_t from) {
        size_t pos = 0;
        for (size_t i = 0; i < from; ++i) {
            pos = line.find_first_not_of(" \t", pos);
            if (pos == std::string::npos) return std::string();
            pos = line.find_first_of(" \t", pos);
            if (pos == std::string::npos) return std::string();
        }
        return line.substr(pos + 1);  // after a single separator
    };
    auto num = [&](size_t i, int def, int lo, int hi) {
        try { return a.size() > i ? std::clamp(std::stoi(a[i]), lo, hi) : def; } catch (...) { return def; }
    };
    auto need = [&](Role r) {
        if (s->role >= r) return true;
        error(s, std::string("Necesitas rol ") + roleName(r) + " para eso");
        return false;
    };
    auto target = [&](size_t i) -> SessionPtr {
        if (a.size() <= i) { error(s, "Falta el nick"); return nullptr; }
        auto t = byNick(a[i]);
        if (!t) error(s, "No hay nadie llamado '" + a[i] + "'");
        // A mod can't act on another mod or an admin; an admin can only act on another admin if they are the owner
        // (logged in with the server's admin password)
        else if (t != s && t->role >= s->role && !s->owner) { error(s, "No puedes hacer eso a alguien de tu mismo rango"); return nullptr; }
        return t;
    };
    // For orders about accounts (also disconnected ones): is it another admin's account?
    auto protectedAccount = [&](const std::string& nick) {
        auto acct = db_.account(nick);
        return acct && acct->role == Role::Admin && !iequals(acct->nick, s->nick) && !canActOnAdmin(s);
    };
    auto startHash = [&](const std::string& p, auto done) {
        if (s->hashBusy) return error(s, "Espera a que termine la operación de contraseña anterior");
        auto salt = Db::newSalt();
        s->hashBusy = true;
        if (!hashAsync(s->ipGroup, p, salt, Db::kArgonKiB, Db::kArgonPasses,
                       [s, salt, done](const std::vector<uint8_t>& h) {
                           s->hashBusy = false;
                           done(salt, h);
                       })) {
            s->hashBusy = false;
            error(s, "El servidor está ocupado: vuelve a intentarlo en unos segundos");
        }
    };
    auto goodPass = [&](const std::string& p) {
        if (p.size() >= 8 && p.size() <= 128) return true;
        error(s, "La contraseña debe tener entre 8 y 128 caracteres");
        return false;
    };

    if (cmd == "pm") {
        if (a.size() < 3) return error(s, "Uso: /pm <nick> <mensaje>");
        std::string text = rest(2);
        if (text.empty() || text[0] != kSealedPrefix) return error(s, "Solo se aceptan mensajes cifrados: actualiza Eco");
        if (text.size() > kMaxSealedText) return error(s, "Mensaje demasiado largo");
        auto t = byNick(a[1]);
        auto acct = db_.account(a[1]);
        if (!t && !acct) return error(s, "No hay nadie llamado '" + a[1] + "'");
        std::string to = t ? t->nick : acct->nick;
        // Limited mailbox: nobody can fill the server disk (or bury other people's messages)
        if (acct && !t && db_.pendingCount(to) >= Db::kMaxPending)
            return error(s, "El buzón de " + to + " está lleno: tendrá que entrar para vaciarlo");
        // Only private messages addressed to accounts are saved: anyone can use a guest nick
        if (acct) db_.addPrivateMessage(s->nick, s->registered, to, text, t != nullptr);
        if (t) {
            t->send(Msg::ChatMsg, w([&](Writer& x) { x.str(""); x.str(s->nick); x.str(text); x.u8(1); }));
            // Confirmation to the sender (private=2: the first field is the recipient)
            if (t != s) s->send(Msg::ChatMsg, w([&](Writer& x) { x.str(to); x.str(s->nick); x.str(text); x.u8(2); }));
        } else {
            s->send(Msg::ChatMsg, w([&](Writer& x) { x.str(to); x.str(s->nick); x.str(text); x.u8(2); }));
            info(s, to + " no está conectado: verá tu mensaje cuando entre");
        }
    } else if (cmd == "key") {
        // Someone's public key, to write them a private message even if they're not connected
        if (a.size() < 2) return;
        std::vector<uint8_t> key;
        std::string nick = a[1];
        if (auto t = byNick(a[1])) { key.assign(t->pubkey, t->pubkey + 32); nick = t->nick; }
        else if (auto acct = db_.account(a[1])) { key = db_.pubkey(a[1]); nick = acct->nick; }
        bool found = key.size() == 32;
        key.resize(32);
        s->send(Msg::PeerKey, w([&](Writer& x) { x.str(nick); x.u8(found ? 1 : 0); x.raw(key.data(), 32); }));
    } else if (cmd == "register") {
        if (s->registered) return error(s, "Ya tienes cuenta. Usa /clave para cambiar la contraseña");
        std::string p = rawRest(1);
        if (!goodPass(p)) return;
        startHash(p, [this, s](const std::vector<uint8_t>& salt, const std::vector<uint8_t>& h) {
            if (s->isClosed() || s->registered) return;
            if (!db_.createAccount(s->nick, salt, h, s->role)) return error(s, "No se pudo crear la cuenta");
            s->registered = true;
            db_.setPubkey(s->nick, std::vector<uint8_t>(s->pubkey, s->pubkey + 32));
            logf(s->nick + T(" registered their account"));
            info(s, "Cuenta creada. Desde ahora el nick " + s->nick +
                        " es tuyo: entra con --clave o escríbela cuando se pida");
            broadcastState();
        });
    } else if (cmd == "passwd") {
        if (!s->registered) return error(s, "Primero crea tu cuenta con /registrar <clave>");
        std::string p = rawRest(1);
        if (!goodPass(p)) return;
        startHash(p, [this, s](const std::vector<uint8_t>& salt, const std::vector<uint8_t>& h) {
            if (s->isClosed()) return;
            if (db_.setPassword(s->nick, salt, h)) info(s, "Contraseña cambiada");
            else error(s, "No se pudo cambiar la contraseña");
        });
    } else if (cmd == "history") {
        int n = num(1, 50, 1, 200);
        auto msgs = db_.history(s->channel, n);
        if (msgs.empty()) return info(s, "Este canal aún no tiene mensajes guardados");
        sendHistory(s, "Historial de " + s->channel, s->channel, msgs);
    } else if (cmd == "pmhistory") {
        if (!s->registered) return error(s, "El historial de privados es solo para cuentas registradas (/registrar)");
        auto msgs = db_.privates(s->nick, num(1, 50, 1, 200));
        if (msgs.empty()) return info(s, "No tienes mensajes privados guardados");
        sendHistory(s, "Tus mensajes privados", "", msgs);
    } else if (cmd == "login") {
        if (a.size() < 2) return error(s, "Uso: /login <clave>");
        if (tooManyFails(s)) return error(s, "Demasiados intentos fallidos. Espera unos minutos.");
        Role nr;
        if (!cfg_.adminPass.empty() && secretEquals(a[1], cfg_.adminPass)) {
            nr = Role::Admin;
            s->owner = true;
            if (masterKey_) {
                // The temporary master password only works once: from here on there is an admin
                crypto_wipe(&cfg_.adminPass[0], cfg_.adminPass.size());
                cfg_.adminPass.clear();
                masterKey_ = false;
                logf(T("Master password used by ") + s->nick + T(": no longer valid"));
                if (!s->registered) info(s, "La clave maestra ya no vale: registra tu cuenta (/registrar) para conservar el rol de admin");
            }
        } else if (!cfg_.modPass.empty() && secretEquals(a[1], cfg_.modPass)) nr = Role::Mod;
        else { noteFail(s); return error(s, "Clave incorrecta"); }
        if (nr < s->role) return info(s, std::string("Ya eres ") + roleName(s->role));
        s->role = nr;
        if (s->registered) db_.setRole(s->nick, nr);
        s->send(Msg::Role, w([&](Writer& x) { x.u8(uint8_t(nr)); }));
        info(s, std::string("Ahora eres ") + roleName(nr) +
                    (s->registered ? " (guardado en tu cuenta)" : ". Usa /registrar <clave> para conservarlo"));
        logf(s->nick + T(" rose to ") + T(roleLabel(nr)));
        broadcastState();
    } else if (cmd == "kick") {
        if (!need(Role::Mod)) return;
        auto t = target(1);
        if (!t) return;
        std::string why = a.size() > 2 ? stripControl(rest(2)) : "Expulsado por " + s->nick;
        truncateUtf8(why, 200);
        logf(s->nick + T(" kicked ") + t->nick);
        infoChannel(t->channel, t->nick + " fue expulsado (" + why + ")");
        kick(t, why);
    } else if (cmd == "ban") {
        if (!need(Role::Admin)) return;
        // An account that isn't connected: the account is banned (there's no IP to ban)
        if (a.size() >= 2 && !byNick(a[1])) {
            auto acct = db_.account(a[1]);
            if (!acct) return error(s, "No hay nadie llamado '" + a[1] + "'");
            if (iequals(acct->nick, s->nick)) return error(s, "No puedes banearte a ti mismo");
            if (protectedAccount(acct->nick)) return;
            banAccounts_.insert(lower(acct->nick));
            db_.addSanction("ban_cuenta", acct->nick, acct->nick, s->nick);
            logf(s->nick + T(" banned the account ") + acct->nick);
            return info(s, "Cuenta baneada: " + acct->nick + " (no podrá entrar con ella)");
        }
        auto t = target(1);
        if (!t) return;
        if (t == s) return error(s, "No puedes banearte a ti mismo");
        if (t->registered) {  // the account too: changing IP isn't enough to come back
            banAccounts_.insert(lower(t->nick));
            db_.addSanction("ban_cuenta", t->nick, t->nick, s->nick);
        }
        // On IPv6 the whole /64 is banned: inside it the address can be changed at will
        const std::string& what = t->ipGroup;
        bans_.insert(what);
        db_.addBan(what, t->nick, s->nick);
        logf(s->nick + T(" banned ") + t->nick + " (" + what + ")");
        info(s, "Baneado " + t->nick + " (IP " + what + (t->registered ? " y su cuenta" : "") + ")");
        kick(t, "Has sido baneado");
        // And any other connection of theirs from that same network
        std::vector<SessionPtr> same;
        for (auto& [id, o] : sessions_)
            if (o != s && o->ipGroup == what) same.push_back(o);
        for (auto& o : same) kick(o, "Has sido baneado");
    } else if (cmd == "unban") {
        if (!need(Role::Admin)) return;
        if (a.size() < 2) return error(s, "Uso: /desbanear <ip o nick>");
        // A banned account, or the exact IP or, on IPv6, any address in its /64
        if (banAccounts_.erase(lower(a[1]))) {
            db_.removeSanction("ban_cuenta", a[1]);
            return info(s, "Cuenta desbaneada: " + a[1]);
        }
        std::string key = bans_.count(a[1]) ? a[1] : ipKey(a[1]);
        bans_.erase(key);
        if (db_.removeBan(key)) info(s, "IP desbaneada: " + key);
        else error(s, "Esa IP o cuenta no estaba baneada");
    } else if (cmd == "bans") {
        if (!need(Role::Admin)) return;
        std::string l = bans_.empty() ? "(ninguna)" : "";
        for (auto& b : bans_) l += b + " ";
        std::string acc, mut;
        for (auto& x : db_.sanctions()) {
            if (x.kind == "ban_cuenta") acc += x.target + " ";
            else mut += x.target + (x.kind == "silencio_ip" ? " (IP) " : " ");
        }
        info(s, "IP baneadas: " + l + " | Cuentas baneadas: " + (acc.empty() ? "(ninguna)" : acc) +
                    " | Silenciados: " + (mut.empty() ? "(nadie)" : mut));
    } else if (cmd == "move") {
        if (!need(Role::Mod)) return;
        if (a.size() < 3) return error(s, "Uso: /mover <nick> <canal>");
        auto t = target(1);
        std::string real;
        if (!t) return;
        if (!channelExists(rest(2), &real)) return error(s, "No existe ese canal");
        moveTo(t, real);
        info(t, "Te movió " + s->nick + " a " + real);
    } else if (cmd == "mute" || cmd == "unmute") {
        if (!need(Role::Mod)) return;
        auto t = target(1);
        if (!t) return;
        t->serverMuted = (cmd == "mute");
        // It is saved: reconnecting doesn't remove it. With an account, per account; if guest, per IP (or /64)
        std::string kind = t->registered ? "silencio_cuenta" : "silencio_ip";
        std::string key = t->registered ? t->nick : t->ipGroup;
        auto& store = t->registered ? muteAccounts_ : muteIps_;
        if (t->serverMuted) {
            store.insert(t->registered ? lower(key) : key);
            db_.addSanction(kind, key, t->nick, s->nick);
        } else {
            store.erase(t->registered ? lower(key) : key);
            db_.removeSanction(kind, key);
        }
        info(s, std::string(t->serverMuted ? "Silenciado " : "Ya puede hablar ") + t->nick +
                    (t->serverMuted ? " (sigue silenciado aunque se reconecte)" : ""));
        info(t, t->serverMuted ? "Un moderador te silenció" : "Ya puedes hablar de nuevo");
        broadcastState();
    } else if (cmd == "mkchan") {
        if (!need(Role::Mod)) return;
        std::string name = rest(1);
        if (name.empty() || name.size() > 32 || !cleanName(name, true))
            return error(s, "Uso: /crearcanal <nombre> (hasta 32 caracteres, sin caracteres invisibles)");
        if (channels_.size() >= 200) return error(s, "Ya hay demasiados canales");
        if (channelExists(name)) return error(s, "Ese canal ya existe");
        db_.addChannel(name);
        channels_ = db_.channels();
        logf(s->nick + T(" created the channel ") + name);
        broadcastState();
    } else if (cmd == "rmchan") {
        if (!need(Role::Admin)) return;
        std::string real;
        if (!channelExists(rest(1), &real)) return error(s, "No existe ese canal");
        if (real == channels_.front()) return error(s, "No se puede borrar el canal principal");
        for (auto& [id, o] : sessions_)
            if (o->authed && o->channel == real) { stopSharing(o, false); o->watching = 0; o->channel = channels_.front(); }
        db_.removeChannel(real);  // also deletes their history
        channels_ = db_.channels();
        logf(s->nick + T(" deleted the channel ") + real);
        broadcastState();
    } else if (cmd == "role") {
        if (!need(Role::Admin)) return;
        if (a.size() < 3) return error(s, "Uso: /rol <nick> <admin|mod|usuario>");
        std::string r = lower(a[2]);
        Role nr = r == "admin" ? Role::Admin : r == "mod" ? Role::Mod : Role::User;
        auto t = byNick(a[1]);
        if (t && t != s && t->role == Role::Admin && !canActOnAdmin(s)) return;
        if (protectedAccount(a[1])) return;
        bool saved = db_.setRole(a[1], nr);
        if (!t && !saved) return error(s, "No hay nadie llamado '" + a[1] + "'");
        if (t) {
            t->role = nr;
            t->send(Msg::Role, w([&](Writer& x) { x.u8(uint8_t(nr)); }));
            info(t, s->nick + " te dio el rol " + roleName(nr));
            broadcastState();
        }
        logf(s->nick + T(" gave the role ") + T(roleLabel(nr)) + T(" to ") + a[1]);
        info(s, a[1] + " ahora es " + roleName(nr) +
                    (saved ? " (guardado)" : " solo hasta que se desconecte: no tiene cuenta registrada"));
    } else if (cmd == "accounts") {
        if (!need(Role::Admin)) return;
        std::string l;
        for (auto& ac : db_.accounts()) l += ac.nick + "(" + roleName(ac.role) + ") ";
        info(s, "Cuentas: " + (l.empty() ? "(ninguna)" : l));
    } else if (cmd == "delaccount") {
        if (!need(Role::Admin)) return;
        if (a.size() < 2) return error(s, "Uso: /borrarcuenta <nick>");
        if (protectedAccount(a[1])) return;
        if (!db_.deleteAccount(a[1])) return error(s, "No existe esa cuenta");
        if (auto t = byNick(a[1])) { t->registered = false; info(t, "Un admin borró tu cuenta"); broadcastState(); }
        logf(s->nick + T(" deleted the account ") + a[1]);
        info(s, "Cuenta borrada: " + a[1]);
    } else if (cmd == "resetpass") {
        if (!need(Role::Admin)) return;
        if (a.size() < 3) return error(s, "Uso: /resetclave <nick> <clave-nueva>");
        if (!goodPass(rawRest(2))) return;
        if (!db_.account(a[1])) return error(s, "No existe esa cuenta");
        if (protectedAccount(a[1])) return;
        std::string who = a[1], by = s->nick;
        startHash(rawRest(2), [this, s, who, by](const std::vector<uint8_t>& salt, const std::vector<uint8_t>& h) {
            if (!db_.setPassword(who, salt, h)) return error(s, "No existe esa cuenta");
            logf(by + T(" changed the password of ") + who);
            info(s, "Contraseña cambiada para " + who);
            // Whoever was inside with the previous password has to log in again
            if (auto t = byNick(who); t && t != s && t->registered) kick(t, "Un admin cambió la contraseña de tu cuenta");
        });
    } else {
        error(s, "Comando desconocido: " + a[0]);
    }
}

}  // namespace

int serverMain(int argc, char** argv) {
    Config cfg;
    // Passwords can also come from environment variables: on the command line any user of the
    // system could see them with "ps". Explicit options take precedence.
    auto env = [](const char* n) { const char* v = std::getenv(n); return v ? std::string(v) : std::string(); };
    cfg.serverPass = env("ECO_SERVER_PASS");
    cfg.adminPass = env("ECO_ADMIN_PASS");
    cfg.modPass = env("ECO_MOD_PASS");
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << T("Missing value for ") << a << "\n"; std::exit(1); }
            return argv[++i];
        };
        try {
            if (a == "--idioma" || a == "--lang" || a == "--language") {
                i18n::Lang l;
                if (!i18n::parse(next(), l)) throw std::invalid_argument("idioma");
                i18n::set(l);
            } else if (a == "--puerto" || a == "--port") {
                int v = std::stoi(next());
                if (v < 1 || v > 65535) throw std::out_of_range("puerto");
                cfg.port = uint16_t(v);
            }
            else if (a == "--pass") cfg.serverPass = next();
            else if (a == "--admin") cfg.adminPass = next();
            else if (a == "--mod") cfg.modPass = next();
            else if (a == "--max") {
                int v = std::stoi(next());
                if (v < 1 || v > 10000) throw std::out_of_range("max");
                cfg.maxUsers = size_t(v);
            }
            else if (a == "--db") cfg.dbFile = next();
            else if (a == "--retener" || a == "--retain") cfg.retainDays = std::stoi(next());
            else if (a == "--copias" || a == "--backups") cfg.backupKeep = std::clamp(std::stoi(next()), 0, 365);
            else if (a == "--actualizaciones" || a == "--updates") cfg.updateDir = next();
            else if (a == "--canales" || a == "--channels") {
                cfg.channels.clear();
                std::stringstream ss(next());
                std::string c;
                while (std::getline(ss, c, ',')) if (!trim(c).empty()) cfg.channels.push_back(trim(c));
                if (cfg.channels.empty()) cfg.channels.push_back("Lobby");
            } else if (a == "--version" || a == "-v") {
                std::cout << T("eco-server ") << kVersion << T(" (protocol ") << kProtoVersion << ")\n";
                return 0;
            } else {
                std::cout << T("Eco ") << kVersion << T(" - server\nUsage: eco-server [--port 9987] [--pass password-to-join] [--admin password] [--mod "
                                                        "password]\n                  [--max 64] [--db eco.db] [--retain days]\n                  "
                                                        "[--channels \"Lobby,General,Games\"]  (only the first time)\n                  [--backups 7]  "
                                                        "daily copies of eco.db in ./copias (0 = none)\n                  [--updates folder]  offer "
                                                        "clients the new signed versions (eco-sign)\nPasswords can also be given with ECO_SERVER_PASS, "
                                                        "ECO_ADMIN_PASS and ECO_MOD_PASS\n(better than on the command line, which other users of the "
                                                        "system can see).\n                  [--language es|en]  language of the console messages "
                                                        "(default: the system's)\neco-server --version  shows the version\n");
                return a == "--help" || a == "-h" ? 0 : 1;
            }
        } catch (const std::exception&) {
            std::cerr << T("Invalid value for ") << a << "\n";
            return 1;
        }
    }
    for (auto* p : {&cfg.adminPass, &cfg.modPass, &cfg.serverPass})
        if (!p->empty() && p->size() < 8)
            std::cout << T("WARNING: one of the server passwords is shorter than 8 characters; it is easy to guess.\n");
    try {
#ifndef _WIN32
        // eco.db holds the server's private identity key and the password hashes: only
        // the user running the server should be able to read it (also the -wal/-shm and the ones that already existed)
        ::umask(077);
        for (const char* suffix : {"", "-wal", "-shm"}) ::chmod((cfg.dbFile + suffix).c_str(), 0600);
#endif
        Db db;
        std::string err;
        if (!db.open(cfg.dbFile, err)) {
            std::cerr << T("Could not open the database ") << cfg.dbFile << ": " << err << "\n";
            return 1;
        }
        asio::io_context io(1);
        Server server(io, cfg, db);
        asio::signal_set sig(io, SIGINT, SIGTERM);
        sig.async_wait([&](std::error_code, int) { logf(T("Shutting down...")); io.stop(); });
        io.run();
    } catch (const std::exception& e) {
        std::cerr << T("Error: ") << e.what() << "\n";
        return 1;
    }
    return 0;
}
