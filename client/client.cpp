// Eco client: voice, text chat, screen sharing and admin commands.
// Used by the console (clientMain, at the end) and the GUI (gui.cpp) through client.hpp.
// Everything travels encrypted to the server and, on top of that, end-to-end encrypted.
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>
#include "../common/i18n.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <thread>

#include <opus/opus.h>

#include "../common/protocol.hpp"
#include "../common/secure.hpp"
#include "../common/update.hpp"
#include "../common/update_key.hpp"
#include "../common/version.hpp"
#include "audio.hpp"
#include "client.hpp"
#include "e2e.hpp"
#include "keys.hpp"
#include "loopback.hpp"
#include "screen.hpp"
#include "screencodec.hpp"
#include "commands.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#endif

using asio::ip::tcp;
using asio::ip::udp;
using namespace eco;

namespace {

std::mutex printMu;
LogSink g_sink;  // empty = console

// Length of the valid UTF-8 sequence starting at s[i] (0 if it is not valid)
size_t utf8SeqLen(const std::string& s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || (n == 2 && c < 0xC2) || i + n > s.size()) return 0;
    for (size_t k = 1; k < n; k++)
        if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
    return n;
}

bool validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        size_t n = utf8SeqLen(s, i);
        if (!n) return false;
        i += n;
    }
    return true;
}

// Everything shown goes through here. System error messages (Windows', the APPDATA
// paths...) come in the ANSI code page and not in UTF-8: the GUI would draw "?" for each accent.
std::string toUtf8(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    if (validUtf8(s)) return s;
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), nullptr, 0);
    if (wn > 0) {
        std::wstring w(size_t(wn), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), &w[0], wn);
        int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
        if (un > 0) {
            std::string u(size_t(un), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &u[0], un, nullptr, nullptr);
            u.erase(std::remove(u.begin(), u.end(), '\r'), u.end());
            return u;
        }
    }
#endif
    // Last resort: interpreted as Latin-1, which always gives valid UTF-8
    std::string out;
    for (size_t i = 0; i < s.size();) {
        size_t n = utf8SeqLen(s, i);
        if (n) { out.append(s, i, n); i += n; continue; }
        unsigned char c = (unsigned char)s[i++];
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    }
    return out;
}

bool consoleMode() {
    std::lock_guard<std::mutex> l(printMu);
    return !g_sink;
}

// Removes what would let another user manipulate the display: control characters (ESC starts
// terminal sequences: move the cursor, erase lines, change the title...), the C1 ones (0x80-0x9F,
// some terminals interpret them the same) and bidi direction marks (they flip the text around).
// keepNewlines: the multi-line blocks the client itself composes; never someone else's text.
std::string safeText(const std::string& in, bool keepNewlines) {
    std::string s = toUtf8(in), out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' && keepNewlines) { out += '\n'; continue; }
        if (c == '\t' || c == '\n' || c == '\r') { out += ' '; continue; }
        if (c < 0x20 || c == 0x7f) continue;
        if (c == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0x9F) {
            ++i;  // C1
            continue;
        }
        if (i + 2 < s.size()) {
            unsigned char d = (unsigned char)s[i + 1], e = (unsigned char)s[i + 2];
            bool bidi = (c == 0xE2 && d == 0x80 && (e == 0x8E || e == 0x8F || (e >= 0xAA && e <= 0xAE))) ||  // LRM RLM LRE..RLO
                        (c == 0xE2 && d == 0x81 && e >= 0xA6 && e <= 0xA9);                                 // LRI..PDI
            if (bidi) { i += 2; continue; }
        }
        if (c == 0xD8 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0x9C) { ++i; continue; }  // ALM
        out += char(c);
    }
    return out;
}

void say(const std::string& raw) {
    std::string s = safeText(raw, false);
    std::lock_guard<std::mutex> l(printMu);
    if (g_sink) return g_sink(s);
    auto t = std::time(nullptr);
    char b[16];
    std::strftime(b, sizeof b, "%H:%M", std::localtime(&t));
    std::cout << "\r" << b << "  " << s << std::endl;
}

// Multi-line block (lists, help, history)
void sayBlock(std::string s) {
    bool nl = !s.empty() && s.back() == '\n';
    s = safeText(s, true);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    if (nl) s += '\n';
    std::lock_guard<std::mutex> l(printMu);
    if (g_sink) {
        while (!s.empty() && s.back() == '\n') s.pop_back();
        return g_sink(s);
    }
    std::cout << "\r" << s << std::flush;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// Hides what is typed (for passwords)
void setEcho(bool on) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD m;
    if (GetConsoleMode(h, &m)) SetConsoleMode(h, on ? (m | ENABLE_ECHO_INPUT) : (m & ~DWORD(ENABLE_ECHO_INPUT)));
#else
    termios t;
    if (tcgetattr(0, &t) == 0) {
        if (on) t.c_lflag |= ECHO;
        else t.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(0, TCSANOW, &t);
    }
#endif
}

// File where the identities of servers already visited are remembered
std::string knownHostsPath() {
#ifdef _WIN32
    const char* d = std::getenv("APPDATA");
    return d ? std::string(d) + "\\eco_servidores.txt" : "eco_servidores.txt";
#else
    const char* d = std::getenv("HOME");
    return d ? std::string(d) + "/.eco_servidores" : "eco_servidores.txt";
#endif
}

std::string fmtTime(uint64_t ts) {
    std::time_t t = std::time_t(ts);
    char b[32];
    std::strftime(b, sizeof b, "%d/%m %H:%M", std::localtime(&t));
    return b;
}

bool readLine(std::string& out) {
#ifdef _WIN32
    // Reads in UTF-16 so accents and ñ arrive fine from the console
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(h, &mode)) {
        wchar_t buf[2048];
        DWORD n = 0;
        if (!ReadConsoleW(h, buf, 2047, &n, nullptr) || n == 0) return false;
        while (n > 0 && (buf[n - 1] == L'\n' || buf[n - 1] == L'\r')) --n;
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, int(n), nullptr, 0, nullptr, nullptr);
        out.assign(size_t(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, int(n), &out[0], len, nullptr, nullptr);
        return true;
    }
#endif
    return bool(std::getline(std::cin, out));
}

const char* helpText() {
    return T("Commands:\n  (text)                  message to the current channel\n  /channels               "
             "list channels and users\n  /join <channel>         switch channel\n  /pm <nick> <text>       "
             "private message (if they have an account and are away, they get it when they log in)\n  "
             "/history [n]            last n messages in the channel (max. 200)\n  /pms [n]                "
             "your saved private messages\n  /register <password>    create your account: reserves your nick "
             "and keeps your role\n  /password <new>         change your password\n  /verify <nick>          "
             "safety code to check that nobody is in the middle\n  /confirm <nick>         mark them as "
             "verified (if their key changes, they get blocked)\n  /invite                 invitation to "
             "this server to pass on to a friend\n  /accept <nick>          accept the new key of a blocked "
             "contact (after checking it)\n  /e2e <phrase> | off     optional: also encrypt with a phrase "
             "shared by the group\n  /mute  /deafen          mute mic / stop hearing others (again to "
             "undo)\n  /ptt <key>|off          push to talk (v, f8, ctrl, alt, mouse4...)\n  /threshold "
             "<0-100>      voice detection sensitivity (default 12)\n  /vol <0-200>            output volume "
             "in %\n  /quality <kbps>         voice bitrate (8-128, default 40)\n  /noise on|off           "
             "mic noise suppression (fan, keyboard...; default on)\n  /agc on|off             automatic gain "
             "for your mic (default on)\n  /volume <nick> <0-300>  volume of one person in % (remembered)\n  "
             "/share [fps] [width] [source] share your screen (5 fps, 1280 px, the main one)\n  /sources     "
             "           what can be shared: each screen and each open window\n  /stopshare              "
             "stop sharing\n  /shareaudio on|off      also send your PC audio while sharing (Windows)\n  "
             "/shareaudio apps        choose which application: /shareaudio app <number>|all\n  /watch "
             "<nick>  /unwatch watch someone's screen / close the viewer\n  /devices                list "
             "microphones and outputs (> marks the one in use)\n  /mic [n|auto]           choose microphone "
             "(auto = the Windows default)\n  /output [n|auto]        choose output: speakers or "
             "headphones\n  /level                  how much the mic is picking up right now\n  /stats       "
             "           network and encryption statistics\n  /ping                   ping to the server and "
             "lost voice\n  /version                Eco and server version\n  /update                 "
             "install the new version offered by the server (signed)\n  /language es|en         Eco's "
             "language: Spanish or English\n  /login <password>       become mod/admin with the server's "
             "password\n  /quit\nModeration (mod): /kick <nick> [reason], /move <nick> <channel>,\n          "
             "        /silence <nick>, /unsilence <nick>, /createchannel <name>\nAdmin:            /ban "
             "<nick>, /unban <ip>, /bans, /deletechannel <name>,\n                  /role <nick> "
             "<admin|mod|user>, /accounts, /deleteaccount <nick>,\n                  /resetpassword <nick> "
             "<password>\n");
}

struct User {
    uint32_t id;
    std::string nick, channel;
    Role role;
    bool sharing, muted, registered;
    bool screenAudio = false;
    // Trust state, computed when the list changes (not on every voice packet)
    bool verified = false, blocked = false;
    PubKey key;
};

class Client : public EcoClient {
public:
    explicit Client(const ClientOptions& o)
        : inputArg(o.input), outputArg(o.output), testTone(o.testTone), trustNewKey(o.trustNewKey), e2ePhrase(o.e2ePhrase),
          screenSink_(o.onScreenFrame), host_(o.host), port_(o.port), nick_(o.nick), spass_(o.serverPass), pass_(o.accountPass),
          tcp_(io_), udp_(io_), pingTimer_(io_), work_(asio::make_work_guard(io_)) {
        rejoin_ = o.rejoinChannel;
        expectedFp_ = normalizeFingerprint(o.expectedFingerprint);
    }
    ~Client() override { shutdown(); }

    std::string inputArg, outputArg;  // --input / --output (number or part of the name)
    bool testTone = false;
    bool trustNewKey = false;  // --trust: accept that the server changed its identity
    std::string e2ePhrase;     // --e2e
    std::function<void(const Image&)> screenSink_;

    bool connect() override {
        try {
            tcp::resolver res(io_);
            auto eps = res.resolve(host_, std::to_string(port_));
            asio::connect(tcp_, eps);
            tcp_.set_option(tcp::no_delay(true));
            auto addr = tcp_.remote_endpoint().address();
            udp_.open(addr.is_v6() ? udp::v6() : udp::v4());
            udp_.bind(udp::endpoint(addr.is_v6() ? udp::v6() : udp::v4(), 0));  // Windows doesn't receive on an unbound socket
            udp_.non_blocking(true);  // ver sendUdp
            srvUdp_ = udp::endpoint(addr, port_);
            std::string err;
            if (!auto_.load(err)) { setError(err); say("! " + err); return false; }
            serverKey_ = lower(host_) + ":" + std::to_string(port_);
            if (!handshake()) return false;
        } catch (const std::exception& e) {
            setError(std::string(T("Could not connect: ")) + e.what());
            say(std::string(T("Could not connect: ")) + e.what());
            return false;
        }
        if (!e2ePhrase.empty()) enableE2E(e2ePhrase);
        sendHello();
        readHeader();
        udpRead();
        net_ = std::thread([this] { io_.run(); });
        return true;
    }

    void shutdown() override {
        if (down_.exchange(true)) return;
        alive_ = false;
        setEcho(true);
        sharing_ = false;
        if (shareT_.joinable()) shareT_.join();
        if (shareAudioT_.joinable()) shareAudioT_.join();
        pttRun_ = false;
        if (pttT_.joinable()) pttT_.join();
        if (viewer_) viewer_->close();
        asio::post(io_, [this] {
            audio_.shutdown();  // audio is always controlled from the network thread
            std::error_code ec;
            tcp_.close(ec);
            udp_.close(ec);
            pingTimer_.cancel();
            work_.reset();
            io_.stop();
        });
        if (net_.joinable()) net_.join();
    }

    bool alive() const override { return alive_; }

    // ---------- Interface for the GUI (client.hpp) ----------
    bool retryable() const override { return retryable_; }
    void startUpdate() override {
        asio::post(io_, [this] { if (alive_) handleInput("/actualizar"); });
    }
    void setShareAudioApp(const std::string& exe) override {
        {
            std::lock_guard<std::mutex> l(appMu_);
            appChoice_ = exe;
        }
        ++appGen_;  // the capture thread reopens it with the new choice
    }
    std::string shareAudioApp() const {
        std::lock_guard<std::mutex> l(appMu_);
        return appChoice_;
    }
    void setScreenVolume(float v) override { audio_.screenVolume = std::max(0.f, std::min(2.f, v)); }

    std::string error() const override {
        std::lock_guard<std::mutex> l(stateMu_);
        return error_;
    }
    std::string hostKeyChanged() const override {
        std::lock_guard<std::mutex> l(stateMu_);
        return newHostKey_;
    }

    void submit(const std::string& line) override {
        if (line.empty() || !alive_) return;
        asio::post(io_, [this, line] { if (alive_) handleInput(line); });
    }

    UiState state() override {
        UiState s;
        {
            std::lock_guard<std::mutex> l(stateMu_);
            s = snap_;
            auto now = std::chrono::steady_clock::now();
            for (auto& u : s.users) {
                auto it = lastVoice_.find(u.id);
                u.talking = it != lastVoice_.end() && now - it->second < std::chrono::milliseconds(300);
            }
        }
        s.alive = alive_;
        s.muted = audio_.muted;
        s.deafened = audio_.deafened;
        s.talking = audio_.talking && !audio_.muted;
        s.pushToTalk = audio_.pushToTalk;
        s.noiseSuppression = audio_.noiseSuppression;
        s.autoGain = audio_.autoGain;
        s.agcGain = audio_.agcGain;
        s.micLevel = audio_.micLevel;
        s.threshold = audio_.vadThreshold;
        s.volume = audio_.volume;
        s.bitrateKbps = audio_.bitrate / 1000;
        s.sharing = sharing_;
        s.shareAudio = shareAudio_;
        s.shareAudioApp = shareAudioApp();
        s.viewers = viewers_;
        s.udpOk = udpOk_;
        s.voiceTcp = tcpVoice_;
        s.pingMs = rttMs_;
        s.lossPct = lossPct_;
        s.updateProgress = updProgress_;
        s.updateReady = updReady_;
        for (auto& u : s.users)
            if (u.me) u.talking = s.talking;
        s.inputName = audio_.inputName();
        s.outputName = audio_.outputName();
        return s;
    }

    void setMuted(bool on) override { audio_.muted = on || audio_.deafened; }
    void setDeafened(bool on) override {
        audio_.deafened = on;
        audio_.muted = on;
    }
    void setVolume(float v) override { audio_.volume = std::max(0.f, std::min(2.f, v)); }
    void setThreshold(float v) override { audio_.vadThreshold = std::max(0.f, std::min(0.1f, v)); }
    void setBitrate(int kbps) override { audio_.bitrate = std::max(8, std::min(128, kbps)) * 1000; }
    void setAutoGain(bool on) override {
        audio_.autoGain = on;
        asio::post(io_, [this] { audio_.saveChoice(); });
    }
    void setUserVolume(const std::string& nick, float v) override {
        asio::post(io_, [this, nick, v] { if (alive_) applyUserVolume(nick, v); });
    }
    void setNoiseSuppression(bool on) override {
        audio_.noiseSuppression = on;
        asio::post(io_, [this] { audio_.saveChoice(); });  // audio choices are saved from the network thread
    }
    void setPushToTalk(const std::string& key) override {
        asio::post(io_, [this, key] { if (alive_) handleInput("/ptt " + (key.empty() ? std::string("off") : key)); });
    }
    void requestDevices() override {
        asio::post(io_, [this] { if (alive_) publishDevices(); });
    }
    void setShareAudio(bool on) override {
        shareAudio_ = on;  // the button already shows the state: no message in the chat
        if (on && !srvScreenAudio_)
            asio::post(io_, [this] { say(T("! This server is from an older version and does not relay screen audio")); });
    }
    void chooseDevice(bool input, const std::string& name) override {
        asio::post(io_, [this, input, name] {
            if (!alive_) return;
            (input ? audio_.inputChoice : audio_.outputChoice) = name;
            audio_.saveChoice();
            reopenAudio(true);
            publishDevices();
        });
    }

    // Console: reads lines until /quit
    void run() {
        std::string line;
        while (alive_ && readLine(line)) {
            if (!alive_) break;
            if (line.empty()) continue;
            if (line == "/salir" || line == "/quit") break;
            asio::post(io_, [this, line] { handleInput(line); });
        }
        shutdown();
    }

private:
    void setError(const std::string& e) {
        std::string u = toUtf8(e);
        std::lock_guard<std::mutex> l(stateMu_);
        error_ = u;
    }

    // Copies the network state (only touched on the network thread) to the snapshot the interface reads
    void publish() {
        std::lock_guard<std::mutex> l(stateMu_);
        snap_.joined = !channel_.empty();
        snap_.nick = nick_;
        snap_.channel = channel_;
        snap_.role = role_;
        snap_.registered = registered_;
        snap_.channels = channels_;
        snap_.users.clear();
        for (auto& ch : channels_)  // in channel order
            for (auto& [id, u] : users_)
                if (u.channel == ch) {
                    UiUser x;
                    x.id = id;
                    x.nick = u.nick;
                    x.channel = u.channel;
                    x.role = u.role;
                    x.sharing = u.sharing;
                    x.screenAudio = u.screenAudio;
                    x.muted = u.muted;
                    x.registered = u.registered;
                    x.me = id == id_;
                    x.volume = userVolume(u.nick);
                    x.verified = u.verified;
                    x.blocked = u.blocked;
                    if (!x.me) x.safetyCode = auto_.safetyCode(u.key);
                    snap_.users.push_back(x);
                }
        snap_.authPrompt = awaitingPass_ ? authMsg_ : "";
        snap_.authSeq = authSeq_;
        snap_.watching = watching_;
        snap_.serverVersion = serverVersion_;
        snap_.updateVersion = updAvail_ ? upd_.version : "";
        snap_.pttKey = audio_.pushToTalk ? pttKey_ : "";
        snap_.e2ePhrase = e2e_.enabled();
        snap_.e2eCheck = e2e_.enabled() ? e2e_.check() : "";
        snap_.invite = inviteText();
        snap_.inviteHasPass = !spass_.empty();
        snap_.serverVerified = serverVerified_;
    }

    // Per-person volume (remembered by nick). Only on the network thread.
    float userVolume(const std::string& nick) const {
        auto it = audio_.nickVolume.find(lower(nick));
        return it == audio_.nickVolume.end() ? 1.f : it->second;
    }
    void applyUserVolume(const std::string& nick, float v) {
        v = std::max(0.f, std::min(3.f, v));
        if (std::fabs(v - 1.f) < 0.01f) audio_.nickVolume.erase(lower(nick));
        else audio_.nickVolume[lower(nick)] = v;
        audio_.saveChoice();
        for (auto& [id, u] : users_)
            if (lower(u.nick) == lower(nick)) audio_.setSenderGain(id, v);
        publish();
    }

    void publishDevices() {
        std::vector<std::string> in, out;
        for (auto& d : audio_.inputs()) in.push_back(d.name);
        for (auto& d : audio_.outputs()) out.push_back(d.name);
        std::lock_guard<std::mutex> l(stateMu_);
        snap_.inputs = std::move(in);
        snap_.outputs = std::move(out);
        snap_.inputChoice = audio_.inputChoice;
        snap_.outputChoice = audio_.outputChoice;
    }

    // ---------- Cifrado ----------
    // Key exchange with the server and check of its identity
    bool handshake() {
        uint8_t esk[32], cpub[32], shared[32];
        secureRandom(esk, 32);
        crypto_x25519_public_key(cpub, esk);
        std::vector<uint8_t> hello = {0, 0, 0, uint8_t(kClientHelloSize), 'E', 'C', 'O', '2'};
        for (int sh = 24; sh >= 0; sh -= 8) hello.push_back(uint8_t(kProtoVersion >> sh));
        hello.insert(hello.end(), cpub, cpub + 32);
        asio::write(tcp_, asio::buffer(hello));

        uint8_t reply[4 + kServerHelloSize];
        asio::read(tcp_, asio::buffer(reply));
        if (reply[0] || reply[1] || reply[2] || reply[3] != kServerHelloSize) {
            setError(T("The server does not speak Eco's protocol (old version?)"));
            say(T("! The server does not speak Eco's protocol (old version?)"));
            return false;
        }
        const uint8_t* spub = reply + 4;
        const uint8_t* idpub = reply + 36;
        auto transcript = handshakeTranscript(kProtoVersion, cpub, spub);
        if (crypto_eddsa_check(reply + 68, idpub, transcript.data(), transcript.size()) != 0) {
            setError(T("Invalid server signature: connection rejected"));
            say(T("! Invalid server signature: connection rejected"));
            return false;
        }
        if (!checkKnownServer(idpub)) return false;
        crypto_x25519(shared, esk, spub);
        deriveKeys(shared, cpub, spub, idpub, keys_);
        crypto_wipe(esk, 32);
        crypto_wipe(shared, 32);
        tx_.key = keys_.tcpC2S;
        rx_.key = keys_.tcpS2C;
        std::memcpy(serverId_, idpub, 32);
        return true;
    }

    // Like SSH: the first time the server identity is remembered; if it changes later, it warns and doesn't connect
    bool checkKnownServer(const uint8_t idpub[32]) {
        std::string key = lower(host_) + ":" + std::to_string(port_), fp = fingerprint(idpub);
        serverFp_ = normalizeFingerprint(fp);
        // With an invitation, the fingerprint is checked right here: not even the first connection is "blind"
        if (!expectedFp_.empty()) {
            if (serverFp_ != expectedFp_) {
                setError(T("The server's identity does NOT match the one in the invitation: someone could be impersonating "
                           "it. Not connected. Ask whoever invited you to send it again."));
                say(T("! The server's identity does not match the one in the invitation. Connection rejected."));
                return false;
            }
            serverVerified_ = true;
        }
        std::string path = knownHostsPath(), line;
        std::vector<std::string> lines;
        bool found = false, changed = false;
        {
            std::ifstream f(path);
            while (std::getline(f, line)) {
                auto sp = line.find(' ');
                if (sp != std::string::npos && line.substr(0, sp) == key) {
                    found = true;
                    if (line.substr(sp + 1) == fp) return true;  // the same as always
                    changed = true;
                    continue;  // replaced if the user accepted it
                }
                if (!line.empty()) lines.push_back(line);
            }
        }
        if (changed && !trustNewKey && !serverVerified_) {  // the invitation already checked it: counts as --trust
            {
                std::lock_guard<std::mutex> l(stateMu_);
                newHostKey_ = fp;
                error_ = T("The server's identity has changed");
            }
            sayBlock(T("\n!!! SECURITY WARNING: the identity of ") + key + T(" has changed.\n    Someone could be impersonating the server to spy on the connection.\n    "
                                                                               "Fingerprint it shows now: ") + fp + T("\n    If the owner reinstalled the server or deleted eco.db, check this fingerprint with "
                                                                 "them\n    and connect again adding --trust\n\n"));
            return false;
        }
        if (serverVerified_) say(T("* Server identity checked against the invitation."));
        else if (!found) say(T("* First connection to this server. Fingerprint: ") + fp + T(" (compare it with the one the server shows)"));
        else say(T("* New server identity accepted: ") + fp);
        lines.push_back(key + " " + fp);
        std::ofstream f(path, std::ios::trunc);
        for (auto& l : lines) f << l << "\n";
        if (!f) say(T("! Could not save the server identity in ") + path);
        return true;
    }

    void enableE2E(const std::string& phrase) {
        if (phrase.size() < 8) return say(T("! The E2E phrase must be at least 8 characters long (several words are better)"));
        if (!e2e_.setPassphrase(phrase, serverId_)) return say(T("! Could not enable E2E"));
        warnedE2E_.clear();
        say(T("* Phrase encryption ON. Check code: ") + e2e_.check() +
            T(" (everyone with the same phrase gets the same one)"));
    }

    void sendHello() {
        Writer w;
        w.str(nick_);
        w.str(pass_);
        w.str(spass_);
        w.raw(auto_.pub().data(), 32);  // my E2E public key
        w.str(kVersion);                // servers older than 2.1 ignore it
        sendTcp(Msg::Hello, w.buf);
    }

    // ---------- Red ----------
    void sendTcp(Msg t, const std::vector<uint8_t>& payload = {}) {
        sendTcpBody(std::make_shared<std::vector<uint8_t>>(body(t, payload)));
    }

    // plain = [u8 type][payload], already built by the caller (screen frames save a copy)
    void sendTcpBody(std::shared_ptr<std::vector<uint8_t>> plain) {
        asio::post(io_, [this, plain] {
            // Encrypted here, on the network thread, so the counter follows the sending order
            auto data = std::make_shared<std::vector<uint8_t>>(tx_.seal(plain->data(), plain->size()));
            pending_ += data->size();
            outq_.push_back(data);
            if (outq_.size() == 1) doWrite();
        });
    }

    void doWrite() {
        asio::async_write(tcp_, asio::buffer(*outq_.front()), [this](std::error_code ec, size_t) {
            if (ec) return lost(T("connection lost"));
            pending_ -= outq_.front()->size();
            outq_.pop_front();
            if (!outq_.empty()) doWrite();
        });
    }

    void readHeader() {
        asio::async_read(tcp_, asio::buffer(hdr_), [this](std::error_code ec, size_t) {
            if (ec) return lost(T("disconnected from the server"));
            uint32_t len = uint32_t(hdr_[0]) << 24 | uint32_t(hdr_[1]) << 16 | uint32_t(hdr_[2]) << 8 | hdr_[3];
            if (len < kMacSize + 1 || len > kMaxFrame) return lost(T("invalid message"));
            body_.resize(len);
            asio::async_read(tcp_, asio::buffer(body_), [this](std::error_code ec, size_t) {
                if (ec) return lost(T("disconnected from the server"));
                if (!rx_.open(body_.data(), body_.size())) return lost(T("tampered data on the connection: closing it for safety"));
                try {
                    Reader r(body_.data() + 1, body_.size() - kMacSize - 1);
                    onMessage(Msg(body_[0]), r);
                } catch (const std::exception& e) {
                    say(std::string(T("* Protocol error: ")) + e.what());
                }
                if (alive_) readHeader();
            });
        });
    }

    void lost(const std::string& why) {
        if (!alive_) return;
        setError(why);
        say("* " + why + (consoleMode() ? T(". Press Enter to quit.") : "."));
        alive_ = false;
    }

    void udpRead() {
        udp_.async_receive_from(asio::buffer(udpBuf_), udpFrom_, [this](std::error_code ec, size_t n) {
            if (ec) { if (alive_ && ec != asio::error::operation_aborted) udpRead(); return; }
            uint64_t ctr;
            // Only packets encrypted with this session's keys and not repeated are accepted
            if (!noUdp_ && udpOpen(keys_.udpS2C, udpBuf_.data(), n, 0, ctr, udpPlain_) && udpRx_.accept(ctr)) onUdp();
            udpRead();
        });
    }

    void onUdp() {
        uint8_t kind = udpPlain_[0];
        if (kind == kUdpPing) {
            auto now = std::chrono::steady_clock::now();
            udpOk_ = true;
            lastPong_ = now;
            auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(now - udpPingAt_).count();
            if (!tcpVoice_ && rtt >= 0 && rtt < 5000) rttMs_ = int(rtt);
            return;
        }
        if (kind == kUdpScreenAudio) return onScreenAudio();
        if ((kind != kUdpVoice && kind != kUdpVoiceE2E && kind != kUdpVoiceAuto) || udpPlain_.size() < 8) return;
        Reader r(udpPlain_.data() + 1, udpPlain_.size() - 1);
        uint32_t from = r.u32();
        uint16_t seq = r.u16();
        if (kind == kUdpVoiceAuto) {
            // Without the sender key (e.g. right when someone joins) the packet is dropped
            auto su = users_.find(from);
            if (su == users_.end() || isBlocked(su->second) ||
                !auto_.openVoice(r.ptr(), r.left(), &su->second.key, voicePlain_))
                return;
            audio_.onPacket(from, seq, voicePlain_.data(), voicePlain_.size());
        } else if (kind == kUdpVoiceE2E) {
            if (!e2e_.openVoice(r.ptr(), r.left(), voicePlain_)) {
                if (warnedE2E_.insert(from).second)
                    say("! " + nickOf(from) + T(" is using E2E encryption and you don't have the same phrase: use /e2e <phrase>"));
                return;
            }
            audio_.onPacket(from, seq, voicePlain_.data(), voicePlain_.size());
        } else {
            // Voice not end-to-end encrypted: no current client sends it, so it can only
            // come from the server (which could pretend to be anyone). It is dropped.
            if (warnedPlain_.insert(from).second)
                say(T("! Discarded unencrypted voice attributed to ") + nickOf(from) + T(" (possible server impersonation)"));
            return;
        }
        std::lock_guard<std::mutex> l(stateMu_);
        lastVoice_[from] = std::chrono::steady_clock::now();  // for the «talking» indicator
    }

    // PC audio of the screen you are watching: signed by whoever shares it and without repeats
    void onScreenAudio() {
        if (udpPlain_.size() < 6) return;
        uint32_t from = uint32_t(udpPlain_[1]) << 24 | uint32_t(udpPlain_[2]) << 16 | uint32_t(udpPlain_[3]) << 8 | udpPlain_[4];
        uint8_t flags = udpPlain_[5];
        if (from != watching_ || !(flags & kScreenAudio)) return;
        const uint8_t* data = udpPlain_.data() + 6;
        size_t len = udpPlain_.size() - 6;
        if (!openScreenBlob(from, flags, data, len)) return;
        try {
            Reader r(data, len);
            uint64_t ctr = r.u64();
            int n = r.u8();
            if (n < 1 || n > kScreenAudioBatch || !screenAudioSeen_.accept(ctr)) return;  // repeated by the server
            for (int i = 0; i < n; ++i) {
                size_t m = 0;
                const uint8_t* opus = r.bytesView16(m);
                ++screenAudioRx_;
                audio_.onScreenAudio(from, uint16_t(ctr + uint64_t(i)), opus, m);
            }
        } catch (const std::exception&) {
        }
    }

    // Voice and screen audio: over UDP or, if it is blocked, over TCP (only from the network thread)
    void sendMedia(const std::vector<uint8_t>& plain) {
        if (!tcpVoice_) return sendUdp(plain);
        if (pending_ > 128 * 1024) return;  // TCP stuck: better to lose 20 ms of voice than to pile up delay
        auto b = std::make_shared<std::vector<uint8_t>>();
        b->reserve(1 + plain.size());
        b->push_back(uint8_t(Msg::VoiceTcp));
        b->insert(b->end(), plain.begin(), plain.end());
        sendTcpBody(std::move(b));
    }

    // ---------- Actualizaciones firmadas ----------
    static bool keyConfigured() {
        for (uint8_t b : kUpdateKey) if (b) return true;
        return false;
    }
    void onUpdateInfo(Reader& r) {
        std::string plat = r.str();
        size_t n = 0, sn = 0;
        const uint8_t* m = r.bytesView(n);
        const uint8_t* sg = r.bytesView(sn);
        if (!keyConfigured() || sn != 64 || plat != update::platform()) return;
        std::string text(reinterpret_cast<const char*>(m), n);
        if (!update::verify(text, sg, kUpdateKey)) {
            say(T("! The server offers an «update» that is not signed by Eco: ignored"));
            return;
        }
        update::Manifest mf;
        if (!update::parse(text, mf) || mf.platform != update::platform()) return;
        if (update::compareVersions(mf.version, kVersion) <= 0) return;  // never to an equal or older version
        upd_ = mf;
        updAvail_ = true;
        say(T("* There is a new version of Eco: ") + mf.version + T(" (yours is ") + kVersion +
            T("). Signature checked. Type /update to install it."));
        publish();
    }

    void beginUpdate() {
        if (updReady_) return say(T("* The new version is already installed: close and reopen Eco"));
        if (!updAvail_) return say(T("* No new version available on this server"));
        if (dl_.active) return say(T("* Already downloading"));
        std::string dir = update::exeDir(), self = std::filesystem::u8path(update::exePath()).filename().u8string();
        if (dir.empty()) return say(T("! Could not find Eco's folder"));
        dl_ = Download{};
        // The Eco programs you have in the folder are updated (and always the one that is open)
        for (size_t i = 0; i < upd_.files.size(); ++i) {
            std::error_code ec;
            const auto& f = upd_.files[i];
            if (f.name == self || std::filesystem::exists(std::filesystem::u8path(dir) / f.name, ec)) {
                dl_.todo.push_back(i);
                dl_.total += f.size;
            }
        }
        if (dl_.todo.empty()) return say(T("! This update does not include any of the programs you have"));
        dl_.dir = dir;
        dl_.active = true;
        say(T("* Downloading Eco ") + upd_.version + "...");
        openNextFile();
    }

    void openNextFile() {
        const auto& f = upd_.files[dl_.todo[dl_.pos]];
        dl_.out.close();
        dl_.out.clear();
        dl_.out.open(std::filesystem::u8path(dl_.dir) / (f.name + ".download"), std::ios::binary | std::ios::trunc);
        if (!dl_.out) return abortUpdate(T("cannot write to Eco's folder (is it in Program Files?)"));
        crypto_blake2b_init(&dl_.hash, 32);
        dl_.off = 0;
        requestChunk();
    }

    void requestChunk() {
        Writer w;
        w.str(update::platform());
        w.str(upd_.files[dl_.todo[dl_.pos]].name);
        w.u32(uint32_t(dl_.off));
        dl_.lastAt = std::chrono::steady_clock::now();
        sendTcp(Msg::UpdateGet, w.buf);
    }

    void onUpdateChunk(Reader& r) {
        if (!dl_.active) return;
        const auto& f = upd_.files[dl_.todo[dl_.pos]];
        std::string name = r.str();
        uint32_t from = r.u32(), total = r.u32();
        size_t n = 0;
        const uint8_t* data = r.bytesView(n);
        if (name != f.name || from != dl_.off || total != f.size || n == 0 || dl_.off + n > f.size)
            return abortUpdate(T("the server sent data that doesn't add up"));
        dl_.out.write(reinterpret_cast<const char*>(data), std::streamsize(n));
        crypto_blake2b_update(&dl_.hash, data, n);
        dl_.off += n;
        dl_.done += n;
        updProgress_ = float(double(dl_.done) / double(dl_.total));
        if (dl_.off < f.size) return requestChunk();
        dl_.out.close();
        uint8_t h[32];
        crypto_blake2b_final(&dl_.hash, h);
        if (!dl_.out || crypto_verify32(h, f.hash) != 0) return abortUpdate(f.name + T(" does not match what was signed"));
        if (++dl_.pos < dl_.todo.size()) return openNextFile();
        installUpdate();
    }

    // Everything downloaded and checked: each program is swapped for the new one. The open one can't be
    // deleted on Windows, but it can be renamed: it stays as .old and is deleted the next time Eco starts
    void installUpdate() {
        namespace fs = std::filesystem;
        fs::path dir = fs::u8path(dl_.dir);
        for (size_t i : dl_.todo) {
            const auto& f = upd_.files[i];
            std::error_code ec;
            fs::path target = dir / f.name, old = dir / (f.name + ".old"), tmp = dir / (f.name + ".download");
            fs::remove(old, ec);
            if (fs::exists(target, ec)) fs::rename(target, old, ec);
            if (ec) return abortUpdate(T("could not move aside ") + f.name + ": " + ec.message());
            fs::rename(tmp, target, ec);
            if (ec) {
                std::error_code ec2;
                fs::rename(old, target, ec2);  // left as it was
                return abortUpdate(T("could not install ") + f.name + ": " + ec.message());
            }
#ifndef _WIN32
            fs::permissions(target, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                        fs::perms::others_read | fs::perms::others_exec, ec);
#endif
        }
        dl_.active = false;
        updProgress_ = -1;
        updReady_ = true;
        say(T("* Eco ") + upd_.version + T(" installed. Close and reopen Eco to use it."));
        publish();
    }

    void abortUpdate(const std::string& why) {
        namespace fs = std::filesystem;
        dl_.out.close();
        std::error_code ec;
        for (size_t i : dl_.todo) fs::remove(fs::u8path(dl_.dir) / (upd_.files[i].name + ".download"), ec);
        dl_.active = false;
        updProgress_ = -1;
        say(T("! Could not update: ") + why);
        publish();
    }

    // Warning if the server is from another version (they tell each other the version when connecting since 2.1)
    static std::vector<int> versionParts(const std::string& v) {
        std::vector<int> p;
        int cur = -1;
        for (char ch : v) {
            if (ch >= '0' && ch <= '9') cur = (cur < 0 ? 0 : cur) * 10 + (ch - '0');
            else if (ch == '.') { p.push_back(cur < 0 ? 0 : cur); cur = -1; }
            else break;
        }
        if (cur >= 0) p.push_back(cur);
        while (p.size() < 3) p.push_back(0);
        return p;
    }
    static std::string fmtPct(float v) {
        char b[16];
        std::snprintf(b, sizeof b, "%.1f %%", double(v));
        return b;
    }
    void warnVersion() {
        if (serverVersion_.empty()) {
            say(T("! The server runs a version of Eco older than 2.1: you won't have screen audio or voice over "
                  "TCP. Ask whoever runs it to update it."));
            return;
        }
        auto s = versionParts(serverVersion_), m = versionParts(kVersion);
        if (s < m)
            say(T("! The server runs Eco ") + serverVersion_ + T(", older than yours (") + kVersion +
                T("): some new features may be missing."));
        else if (s > m)
            say(T("* The server runs Eco ") + serverVersion_ + T(", newer than yours (") + kVersion + ").");
    }

    // Only from the network thread (the counter must not repeat)
    void sendUdp(const std::vector<uint8_t>& plain) {
        if (noUdp_) return;
        uint8_t prefix[4] = {uint8_t(id_ >> 24), uint8_t(id_ >> 16), uint8_t(id_ >> 8), uint8_t(id_)};
        // Direct send with the socket in non-blocking mode and a reused buffer: UDP almost never
        // has to wait and, if the system buffer were full, for voice it is better to lose the packet
        // than to send it late. This way no memory is allocated and no handler is queued 50 times per second.
        udpSealInto(udpSeal_, keys_.udpC2S, udpTx_++, prefix, 4, plain.data(), plain.size());
        std::error_code ec;
        udp_.send_to(asio::buffer(udpSeal_), srvUdp_, 0, ec);
    }

    void ping() {
        auto now = std::chrono::steady_clock::now();
        udpPingAt_ = now;
        sendUdp({kUdpPing});
        // UDP without response: voice switches to TCP (more delay, but it works on networks that block UDP).
        // UDP keeps being tried and, as soon as it responds, it switches back.
        if (udpOk_ && now - lastPong_ > std::chrono::seconds(16)) udpOk_ = false;
        if (id_ && (srvFeatures_ & kFeatTcpVoice)) {
            if (!udpOk_ && !tcpVoice_ && now - welcomeAt_ > std::chrono::seconds(3)) {
                tcpVoice_ = true;
                sendTcp(Msg::TcpVoiceMode, {1});
                say(T("! UDP is not responding (firewall or corporate network?): voice goes over TCP, with a bit more delay"));
            } else if (udpOk_ && tcpVoice_) {
                tcpVoice_ = false;
                sendTcp(Msg::TcpVoiceMode, {0});
                say(T("* UDP works again: voice goes over UDP again"));
            }
        }
        if (dl_.active && now - dl_.lastAt > std::chrono::seconds(20)) abortUpdate(T("the server stopped sending it"));
        // Voice lost in the last 5 s (only if enough arrived for the figure to mean something)
        uint64_t played = audio_.playedFrames, lost = audio_.lostFrames;
        uint64_t dp = played - lastPlayed_, dl = lost - lastLost_;
        lastPlayed_ = played;
        lastLost_ = lost;
        if (dp + dl >= 25) lossPct_ = float(100.0 * double(dl) / double(dp + dl));
        else if (dp + dl == 0) lossPct_ = -1;
        if (tcpVoice_ && (srvFeatures_ & kFeatPong)) {  // over TCP the ping is measured with Ping/Pong
            Writer pw;
            pw.u64(uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()));
            sendTcp(Msg::Ping, pw.buf);
        } else if (++pingCount_ % 3 == 0) {
            sendTcp(Msg::Ping);
        }
        // At first, every second: that way we know right away whether UDP works; then, every 5 s (plenty for
        // the router not to forget the connection: they usually take 30 s or more)
        bool early = id_ && now - welcomeAt_ < std::chrono::seconds(8);
        pingTimer_.expires_after(early ? std::chrono::seconds(1) : std::chrono::seconds(5));
        pingTimer_.async_wait([this](std::error_code ec) { if (!ec && alive_) ping(); });
    }

    // ---------- Messages from the server ----------
    void onMessage(Msg type, Reader& r) {
        switch (type) {
            case Msg::Welcome: {
                id_ = r.u32();
                uint16_t up = r.u16();
                role_ = Role(r.u8());
                bool reg = r.u8() != 0;
                srvFeatures_ = r.left() >= 1 ? r.u8() : 0;
                srvScreenAudio_ = (srvFeatures_ & kFeatScreenAudio) != 0;
                serverVersion_ = r.left() >= 2 ? cleanVersion(r.str()) : "";
                welcomeAt_ = std::chrono::steady_clock::now();
                registered_ = reg;
                awaitingPass_ = false;
                srvUdp_.port(up);
                say(T("* Eco ") + std::string(kVersion) + T(", server ") +
                    (serverVersion_.empty() ? std::string(T("older than 2.1")) : serverVersion_));
                warnVersion();
                if (srvFeatures_ & kFeatUpdates) {  // is there a new version published?
                    Writer uw;
                    uw.str(update::platform());
                    sendTcp(Msg::UpdateQuery, uw.buf);
                }
                if (!rejoin_.empty()) {  // reconnection: back to the channel you were in
                    Writer jw;
                    jw.str(rejoin_);
                    sendTcp(Msg::Join, jw.buf);
                    joinPending_ = true;
                }
                say(T("Connected as ") + nick_ + " (" + T(roleLabel(role_)) + (reg ? T(", registered account") : T(", guest")) +
                    T("). Type /help to see commands."));
                say(T("* End-to-end encryption on: not even the server can read the chat, hear the voice or see the screen."));
                ping();
                audio_.testTone = testTone;
                audio_.loadChoice();
                if (!inputArg.empty()) audio_.inputChoice = deviceArg(true, inputArg);
                if (!outputArg.empty()) audio_.outputChoice = deviceArg(false, outputArg);
                audio_.onDevicesChanged = [this] {
                    asio::post(io_, [this] { if (alive_) reopenAudio(false); });
                };
                bool ok = audio_.start(
                    [this](const uint8_t* p, size_t n) {
                        // Capture thread: audio is end-to-end encrypted here, before it leaves
                        Writer w;
                        std::vector<uint8_t> sealed;
                        bool phrase = e2e_.enabled();
                        bool ok = phrase ? e2e_.sealVoice(p, n, sealed) : auto_.sealVoice(p, n, sealed);
                        if (!ok) return;  // voice is never sent unencrypted
                        w.u8(phrase ? kUdpVoiceE2E : kUdpVoiceAuto);
                        w.u16(seq_++);
                        w.raw(sealed.data(), sealed.size());
                        auto plain = std::make_shared<std::vector<uint8_t>>(std::move(w.buf));
                        asio::post(io_, [this, plain] { if (alive_) sendMedia(*plain); });
                    });
                audioNotes();
                if (!ok) say(T("* Audio not available for now: if you plug in headphones they will open on their own. See /devices"));
                else sayDevices();
                publish();
                publishDevices();
                break;
            }
            case Msg::State: onState(r); break;
            case Msg::ChatMsg: {
                std::string ch = r.str(), from = r.str(), text = r.str();
                uint8_t priv = r.u8();
                if (priv == 2) {  // confirmation of a private message I sent; "ch" carries the recipient
                    say(T("[private to ") + ch + "] " + readable(text, adPm(nick_, ch), ch));
                    break;
                }
                // Arriving now: a message already seen or with an old key is a replay from the server
                bool self = lower(from) == lower(nick_);
                bool replay = false;
                text = priv ? readable(text, adPm(from, nick_), from, "", !self, &replay)
                            : readable(text, adChat(from, ch), "", from, true, &replay);
                if (replay) {
                    say(T("! Discarded a repeated message attributed to ") + from + T(" (the server resent it)"));
                    break;
                }
                say(priv ? T("[private] ") + from + ": " + text : from + ": " + text);
                break;
            }
            case Msg::AuthFail:
                authMsg_ = safeText(i18n::fromServer(r.str()), false);
                ++authSeq_;
                say("* " + authMsg_);
                awaitingPass_ = true;
                setEcho(false);
                publish();
                break;
            case Msg::History: {
                std::string title = i18n::fromServer(r.str()), ch = r.str();
                int n = r.u16();
                std::string out = "-- " + title + " --\n";
                int unreadable = 0;  // from before you joined: summarized in one line instead of one per message
                for (int i = 0; i < n; ++i) {
                    uint64_t ts = r.u64();
                    std::string from = r.str(), to = r.str(), text = r.str();
                    if (to.empty() && Keys::isGroup(text)) {
                        PubKey sk;
                        bool self = lower(from) == lower(nick_);
                        bool known = self ? (sk = auto_.pub(), true) : keyOfNick(from, sk);
                        std::string plain;
                        if (!auto_.openText(text, adChat(from, ch), known ? &sk : nullptr, plain)) {
                            ++unreadable;
                            continue;
                        }
                    }
                    text = to.empty() ? readable(text, adChat(from, ch), "", from)
                                      : readable(text, adPm(from, to), lower(from) == lower(nick_) ? to : from);
                    out += "  [" + fmtTime(ts) + "] " + safeText(from, false) + (to.empty() ? "" : " -> " + safeText(to, false)) +
                           ": " + text + "\n";
                }
                if (unreadable)
                    out += "  (" + std::to_string(unreadable) + (unreadable == 1 ? T(" message") : T(" messages")) +
                           T(" from before you joined: they are encrypted only for those who were there, so they can't be "
                             "read here. This is normal.)\n");
                if (unreadable == n) {  // nothing readable: just the note, without a title
                    if (n) say("* (" + std::to_string(n) + T(" earlier messages from ") + ch +
                               T(" can't be read here: they are encrypted only for those who were there. This is normal.)"));
                    break;
                }
                sayBlock(out + "--\n");
                break;
            }
            case Msg::KeyFrom: {
                // Someone in the channel hands me their sender key, encrypted only for me
                uint32_t from = r.u32();
                auto blob = r.bytes();
                auto it = users_.find(from);
                if (it != users_.end()) auto_.unwrapFrom(it->second.key, blob.data(), blob.size());
                break;
            }
            case Msg::PeerKey: {
                std::string nick = r.str();
                bool found = r.u8() != 0;
                PubKey key;
                const uint8_t* kp = r.ptr();
                r.skip(32);  // checks that 32 bytes really come before copying them
                std::memcpy(key.data(), kp, 32);
                auto pend = pendingPm_.find(lower(nick));
                if (pend == pendingPm_.end()) break;
                if (!found) {
                    say(T("! Can't write to '") + nick + T("': doesn't exist or hasn't logged in with this version yet"));
                } else if (auto_.blocked(serverKey_, nick, key)) {
                    noteKey(nick, key, true);
                    say(T("! Your private message was NOT sent to ") + nick + T(": their key is not the one you verified. Check it with /verify ") + nick + T(" and, if it is correct, /accept ") + nick);
                } else {
                    noteKey(nick, key, true);
                    for (auto& text : pend->second) {
                        std::string sealed = auto_.sealPm(text, key, adPm(nick_, nick));
                        if (sealed.empty()) { say(T("! Could not encrypt the private message for ") + nick + T(" (their key is not valid or the encryption state could not be saved to disk)")); break; }
                        cmd("pm " + nick + " " + sealed);
                    }
                }
                pendingPm_.erase(pend);
                break;
            }
            case Msg::Info: say("* " + i18n::fromServer(r.str())); break;
            case Msg::Error:
                say("! " + i18n::fromServer(r.str()));
                if (joinPending_) flushHeldChat();  // e.g. the channel doesn't exist: stays where it was
                break;
            case Msg::UpdateInfo: onUpdateInfo(r); break;
            case Msg::UpdateChunk: onUpdateChunk(r); break;
            case Msg::VoiceFrom:  // voice or screen audio arriving over TCP because your UDP doesn't work
                udpPlain_.assign(r.ptr(), r.ptr() + r.left());
                if (!udpPlain_.empty()) onUdp();
                break;
            case Msg::Pong: {
                uint64_t sent = r.u64();
                uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch()).count());
                if (tcpVoice_ && now >= sent && now - sent < 10000) rttMs_ = int(now - sent);
                break;
            }
            case Msg::Kicked: {
                retryable_ = false;
                std::string why = T("Disconnected: ") + safeText(i18n::fromServer(r.str()), false);
                setError(why);
                say("* " + why);
                alive_ = false;
                break;
            }
            case Msg::Role:
                role_ = Role(r.u8());
                publish();
                break;
            case Msg::KeyRequest: keyReq_ = true; break;
            case Msg::Viewers: {
                int n = r.u16();
                if (n != viewers_) say(T("* Watching your screen: ") + std::to_string(n));
                viewers_ = n;
                if (n > 0) keyReq_ = true;
                break;
            }
            case Msg::ScreenFrom: {
                uint32_t from = r.u32();
                if (from != watching_ || (!viewer_ && !screenSink_)) return;
                uint8_t flags = r.u8();
                const uint8_t* data = r.ptr();
                size_t len = r.left();
                if ((flags & kScreenAudio) || !openScreenBlob(from, flags, data, len)) return;
                if (flags & kScreenCounter) {  // repeated by the server (or old): out
                    if (len < 8) return;
                    uint64_t fc = 0;
                    for (size_t i = len - 8; i < len; ++i) fc = fc << 8 | data[i];
                    if (frameSeenAny_ && fc <= frameSeen_) return;
                    frameSeen_ = fc;
                    frameSeenAny_ = true;
                    len -= 8;
                }
                if (!decoder_.apply(data, len, canvas_)) return;
                ++framesRx_;
                bytesRx_ += len;
                if (screenSink_) {
                    screenSink_(canvas_);
                    return;
                }
                if (!viewer_->isOpen()) {  // the user closed the window
                    stopWatching();
                    return;
                }
                viewer_->present(canvas_);
                break;
            }
            default: break;
        }
    }

    // Decrypts a screen frame or screen audio; if it works, data/len point to the plaintext content
    bool openScreenBlob(uint32_t from, uint8_t flags, const uint8_t*& data, size_t& len) {
        if (flags & kScreenAuto) {
            auto su = users_.find(from);
            if (su == users_.end() || isBlocked(su->second) ||
                !auto_.openBlob(data, len, flags, (flags & kScreenSigned) != 0, &su->second.key, screenPlain_))
                return false;
        } else if (flags & kScreenE2E) {
            if (!e2e_.openBlob(data, len, flags, screenPlain_)) {
                if (warnedE2E_.insert(from | 0x80000000u).second)
                    say(T("! That screen is E2E encrypted and you don't have the same phrase: use /e2e <phrase>"));
                return false;
            }
        } else {
            // Unencrypted: no current client sends it; it could only come from the server
            if (warnedPlain_.insert(from | 0x80000000u).second)
                say(T("! Discarded an unencrypted screen attributed to ") + nickOf(from));
            return false;
        }
        data = screenPlain_.data();
        len = screenPlain_.size();
        return true;
    }

    void onState(Reader& r) {
        std::map<uint32_t, User> users;
        std::vector<std::string> chans;
        int nc = r.u16();
        for (int c = 0; c < nc; ++c) {
            std::string ch = r.str();
            chans.push_back(ch);
            int nu = r.u16();
            for (int u = 0; u < nu; ++u) {
                User x;
                x.id = r.u32();
                x.nick = r.str();
                x.role = Role(r.u8());
                uint8_t f = r.u8();
                x.sharing = f & kUserSharing;
                x.muted = f & kUserMuted;
                x.registered = f & kUserRegistered;
                x.screenAudio = f & kUserScreenAudio;
                const uint8_t* kp = r.ptr();
                r.skip(32);  // first the length is checked, then it is copied
                std::memcpy(x.key.data(), kp, 32);
                x.channel = ch;
                users[x.id] = x;
            }
        }
        std::string oldCh = channel_;
        users_ = std::move(users);
        channels_ = std::move(chans);
        auto me = users_.find(id_);
        if (me != users_.end()) {
            channel_ = me->second.channel;
            role_ = me->second.role;
            nick_ = me->second.nick;  // as the server has it registered
        }
        // If someone's key is not the one remembered, a warning is shown (like WhatsApp's warning)
        for (auto& [id, u] : users_)
            if (id != id_) {
                noteKey(u.nick, u.key, u.registered);
                audio_.setSenderGain(id, userVolume(u.nick));  // the volume you gave them last time
            }
        // Every time who is in the channel changes, a new sender key is created and handed out:
        // whoever joins can't read what came before and whoever leaves can't read what comes next
        refreshTrust();
        shareKeys(channel_ != oldCh);
        if (channel_ != oldCh) {
            audio_.clearSenders();
            if (sharing_) stopSharing(false);
            say(T("== Channel: ") + channel_ + " ==");
            printChannel();
            if (joinPending_ || oldCh.empty()) flushHeldChat();  // already with the new channel and key
        }
        if (watching_) {
            auto w = users_.find(watching_);
            if (w == users_.end() || !w->second.sharing || w->second.channel != channel_) stopWatching();
        }
        if (me != users_.end()) registered_ = me->second.registered;
        publish();
    }

    // Creates a new sender key and hands it to the channel members if they changed (or if forced)
    void shareKeys(bool force) {
        // A verified contact whose key changed is not given the sender key: if it is an
        // impostor planted by the server, it won't be able to read the channel
        std::vector<std::pair<uint32_t, PubKey>> members;
        for (auto& [id, u] : users_)
            if (id != id_ && u.channel == channel_) {
                if (isBlocked(u)) {
                    if (warnedBlocked_.insert(lower(u.nick)).second)
                        say("! " + u.nick + T(" is blocked: their key is not the one you verified. They won't be able to read what you write "
                                              "and you won't hear them. Check it with /verify ") + u.nick + T(" and then /accept ") + u.nick);
                    continue;
                }
                members.push_back({id, u.key});
            }
        if (!channel_.empty() && (members != members_ || force)) {
            members_ = members;
            auto_.rotate();
            Writer w;
            w.u16(uint16_t(members.size()));
            for (auto& [id, key] : members) {
                auto blob = auto_.wrapFor(key);
                w.u32(id);
                w.bytes(blob.data(), blob.size());
            }
            if (!members.empty()) sendTcp(Msg::KeyShare, w.buf);
        }
    }

    void printChannel() {
        std::string l = T("   In the channel:");
        for (auto& [id, u] : users_)
            if (u.channel == channel_) l += " " + decorate(u);
        say(l);
    }

    static std::string decorate(const User& u) {
        std::string s = u.nick;
        if (u.role == Role::Admin) s = "@" + s;
        else if (u.role == Role::Mod) s = "+" + s;
        if (u.sharing) s += u.screenAudio ? T("[screen with audio]") : "[pantalla]";
        if (u.muted) s += "[silenciado]";
        return s;
    }

    std::string nickOf(uint32_t id) {
        auto it = users_.find(id);
        return it == users_.end() ? "alguien" : it->second.nick;
    }

    // Data that binds each E2E message to its sender and destination: the server can't
    // change who a message is from or move it to another channel without it being noticed
    static std::string adChat(const std::string& from, const std::string& ch) { return "c|" + lower(from) + "|" + lower(ch); }
    static std::string adPm(const std::string& from, const std::string& to) { return "p|" + lower(from) + "|" + lower(to); }

    // Turns a received text into something readable (decrypts it if it comes with E2E)
    // peer: in a private message, the other person's nick. from: in a channel, who sends it (it is checked
    // that the sender key is theirs). Prints nothing (used with the console locked).
    // live: arriving now (not history). If it is a replay, returns "" and sets *replay.
    // Other people's text comes back already sanitized (no terminal sequences or line breaks).
    std::string readable(const std::string& text, const std::string& ad, const std::string& peer = "",
                         const std::string& from = "", bool live = false, bool* replay = nullptr) {
        std::string plain;
        if (replay) *replay = false;
        if (Keys::isGroup(text)) {
            PubKey sk;
            bool self = lower(from) == lower(nick_);
            bool known = !from.empty() && (self ? (sk = auto_.pub(), true) : keyOfNick(from, sk));
            if (known && !self && auto_.blocked(serverKey_, from, sk))
                return T("[blocked: the key of ") + from + T(" is not the one you verified; /verify ") + from + "]";
            if (auto_.openText(text, ad, known ? &sk : nullptr, plain, live)) return safeText(plain, false);
            // Could it be opened as history? Then what happens is that it is old or repeated
            if (live && auto_.openText(text, ad, known ? &sk : nullptr, plain, false)) {
                if (replay) *replay = true;
                return "";
            }
            return T("[encrypted message this device can't read: it's from before you joined, from another device, "
                     "or its signature is invalid]");
        }
        if (Keys::isPm(text)) {
            PubKey k;
            if (!keyOfNick(peer, k)) return T("[encrypted private message this device can't read]");
            if (lower(peer) != lower(nick_) && auto_.blocked(serverKey_, peer, k))
                return T("[blocked: the key of ") + peer + T(" is not the one you verified; /verify ") + peer + "]";
            bool rep = false;
            if (auto_.openPm(text, k, ad, plain, live, &rep)) return safeText(plain, false);
            if (rep && replay) *replay = true;
            return rep ? "" : T("[encrypted private message this device can't read]");
        }
        // Unencrypted text: no current client sends it (and the server no longer accepts it). It can only
        // come from a malicious server pretending to be someone, so it is not shown.
        if (!E2E::isSealed(text)) return T("[unencrypted message discarded: possible server impersonation]");
        if (!e2e_.enabled()) return T("[end-to-end encrypted message: enable /e2e <phrase> to read it]");
        if (!e2e_.openText(text, ad, plain)) return T("[E2E message that can't be decrypted: different phrase or tampered message]");
        return safeText(plain, false);
    }

    bool isBlocked(const User& u) const { return u.id != id_ && u.blocked; }

    // Recomputes verified/blocked for everyone (when the list changes or after /confirm and /accept)
    void refreshTrust() {
        for (auto& [id, u] : users_) {
            u.verified = id != id_ && auto_.isVerified(serverKey_, u.nick, u.key);
            u.blocked = id != id_ && !u.verified && auto_.blocked(serverKey_, u.nick, u.key);
        }
    }

    std::string inviteText() const {
        if (serverFp_.empty()) return "";
        return makeInvite(host_, port_, serverFp_, spass_);
    }

    bool keyOfNick(const std::string& nick, PubKey& k) {
        for (auto& [id, u] : users_)
            if (lower(u.nick) == lower(nick)) { k = u.key; return true; }
        return auto_.lookup(serverKey_, nick, k);
    }

    // Remembers a contact's key and warns if it is no longer the same
    void noteKey(const std::string& nick, const PubKey& key, bool registered) {
        if (auto_.remember(serverKey_, nick, key) != Keys::Seen::Changed) return;
        if (auto_.blocked(serverKey_, nick, key)) {
            warnedBlocked_.erase(lower(nick));
            say(T("!!! The safety code of ") + nick + T(", whom you had VERIFIED, changed. They are blocked: nothing is encrypted for them and nothing "
                                                               "of theirs is shown. Check it with /verify ") + nick + T(" and, if it's correct, /accept ") +
                nick);
            return;
        }
        say(T("! The safety code of ") + nick + T(" changed") +
            (registered ? T(": they may be using another device or have reinstalled Eco.")
                        : T(": it's a guest nick, it may be someone else.")) +
            T(" Check it with /verify ") + nick);
    }

    // ---------- Audio devices ----------
    // "3" -> name of device 3 in the list; if it is not a number, it is used as part of the name
    std::string deviceArg(bool input, const std::string& arg) {
        bool digits = !arg.empty() && std::all_of(arg.begin(), arg.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
        if (!digits) return arg;
        auto v = input ? audio_.inputs() : audio_.outputs();
        size_t i = size_t(std::atoi(arg.c_str()));
        if (i >= 1 && i <= v.size()) return v[i - 1].name;
        say(T("! There is no ") + std::string(input ? T("microphone") : "salida") + T(" number ") + arg + T("; using the default"));
        return "";
    }

    void audioNotes() {
        for (auto& n : audio_.notes) say(T("! Audio: ") + n);
        audio_.notes.clear();
    }

    void sayDevices() {
        std::string i = audio_.inputName(), o = audio_.outputName();
        say(T("* Microphone: ") + (i.empty() ? std::string("ninguno") : i) + (audio_.inputChoice.empty() ? T(" (auto)") : "") +
            T("  |  Output: ") + (o.empty() ? std::string("ninguna") : o) + (audio_.outputChoice.empty() ? T(" (auto)") : ""));
    }

    // Reopens the audio (e.g. when plugging in headphones) and warns if something changed
    void reopenAudio(bool announce) {
        std::string i0 = audio_.inputName(), o0 = audio_.outputName();
        bool ok = audio_.restart();
        audioNotes();
        if (!ok) say(T("! No audio device available"));
        if (announce || audio_.inputName() != i0 || audio_.outputName() != o0) sayDevices();
    }

    void chooseDeviceCmd(bool input, const std::vector<std::string>& a, const std::string& arg) {
        const char* cmdName = input ? "mic" : (i18n::english() ? "output" : "salida");
        if (a.size() < 2)
            return sayBlock(audio_.describe(input) + T("Use /") + cmdName + T(" <number> or /") + cmdName + T(" auto\n"));
        std::string val = lower(arg) == "auto" ? "" : deviceArg(input, arg);
        if (!val.empty()) {
            auto v = input ? audio_.inputs() : audio_.outputs();
            std::string lv = lower(val);
            bool found = false;
            for (auto& d : v)
                if (d.name == val || lower(d.name).find(lv) != std::string::npos) { found = true; break; }
            if (!found) return say(T("! Can't find '") + arg + T("'. See the list with /") + cmdName);
        }
        (input ? audio_.inputChoice : audio_.outputChoice) = val;
        audio_.saveChoice();
        reopenAudio(true);
        publishDevices();
    }

    User* findUser(const std::string& nick) {
        for (auto& [id, u] : users_)
            if (lower(u.nick) == lower(nick)) return &u;
        return nullptr;
    }

    // ---------- Pantalla ----------
    void startSharing(int fps, int width, const std::string& source = "") {
        if (sharing_) return say(T("! You are already sharing"));
        sharing_ = true;
        int gen = ++shareGen_;  // the threads from a previous time end even if sharing_ becomes true again
        viewers_ = 0;
        sendTcp(Msg::ScreenStart);
        say(T("* Sharing screen at ") + std::to_string(fps) + T(" fps, ") + std::to_string(width) +
            T(" px wide. It is only sent if someone is watching."));
        if (shareT_.joinable()) shareT_.join();
        shareT_ = std::thread([this, fps, width, gen, source] {
            auto cap = ScreenCapturer::create(source);
            if (!cap->backend().empty()) say(T("* Capturing with ") + cap->backend());
            ScreenEncoder enc;
            enc.maxWidth = width;
            Image img;
            std::vector<uint8_t> payload, sealed;  // reused between frames
            auto period = std::chrono::milliseconds(1000 / fps);
            auto next = std::chrono::steady_clock::now();
            bool warned = false;
            while (sharing_ && gen == shareGen_) {
                next += period;
                std::this_thread::sleep_until(next);
                if (std::chrono::steady_clock::now() > next + period * 5) next = std::chrono::steady_clock::now();
                if (viewers_ <= 0) continue;           // nadie mira: cero CPU, cero red
                if (pending_ > 768 * 1024) continue;   // upload saturated: skip the frame
                if (!cap->capture(img)) {
                    if (!warned) {
                        say(source.rfind("ventana:", 0) == 0 ? T("! Could not capture the window (closed or minimized?)")
                                                             : T("! Could not capture the screen"));
                        warned = true;
                    }
                    continue;
                }
                warned = false;
                if (cap->unchanged() && !keyReq_) continue;  // nothing new on screen: neither compare nor encode
                if (enc.encode(img, keyReq_.exchange(false), payload)) {
                    // payload[4] says whether it is a key frame; it is end-to-end encrypted before it leaves
                    // At the end, a counter that never repeats (clients older than 2.2 ignore it)
                    uint64_t fc = frameCtr_++;
                    for (int i = 56; i >= 0; i -= 8) payload.push_back(uint8_t(fc >> i));
                    uint8_t flags = (payload[4] ? kScreenKey : 0) | kScreenCounter |
                                    (e2e_.enabled() ? kScreenE2E : kScreenAuto | kScreenSigned);
                    bool ok = e2e_.enabled() ? e2e_.sealBlob(payload.data(), payload.size(), flags, sealed)
                                             : auto_.sealBlob(payload.data(), payload.size(), flags, sealed);
                    if (!ok) { keyReq_ = true; continue; }  // the screen is never sent unencrypted
                    // Message body built in one go: [type][flags][encrypted frame]
                    auto out = std::make_shared<std::vector<uint8_t>>();
                    out->reserve(2 + sealed.size());
                    out->push_back(uint8_t(Msg::ScreenFrame));
                    out->push_back(flags);
                    out->insert(out->end(), sealed.begin(), sealed.end());
                    ++framesTx_;
                    bytesTx_ += out->size() - 1;
                    sendTcpBody(std::move(out));
                }
            }
        });
        if (shareAudioT_.joinable()) shareAudioT_.join();
        shareAudioT_ = std::thread([this, gen] { shareAudioLoop(gen); });
    }

    // PC audio while sharing: only captured if it is enabled and someone is watching.
    // It goes over UDP in packets of 3 frames (60 ms): one signature per packet instead of one per frame.
    void shareAudioLoop(int gen) {
        int err = 0;
        OpusEncoder* enc = opus_encoder_create(kSampleRate, 2, OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK || !enc) return;
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(96000));
        opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(1));  // no peaks that don't fit in the packet
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(8));
        std::unique_ptr<SystemAudioCapture> cap;
        std::vector<int16_t> acc;
        std::vector<uint8_t> plain, sealed;
        uint8_t pkt[kScreenAudioMaxOpus];
        int frames = 0;        // frames in the packet being filled
        uint64_t first = 0;    // counter of its first frame
        int quiet = 0;
        bool warned = false, warnedSelf = false;
        auto retryAt = std::chrono::steady_clock::now();
        constexpr size_t kStereoFrame = size_t(kFrameSamples) * 2;
        auto flush = [&] {
            if (frames == 0) return;
            plain[8] = uint8_t(frames);
            frames = 0;
            uint8_t flags = kScreenAudio | (e2e_.enabled() ? kScreenE2E : kScreenAuto | kScreenSigned);
            bool ok = e2e_.enabled() ? e2e_.sealBlob(plain.data(), plain.size(), flags, sealed)
                                     : auto_.sealBlob(plain.data(), plain.size(), flags, sealed);
            if (!ok) return;  // never unencrypted
            auto out = std::make_shared<std::vector<uint8_t>>();
            out->reserve(2 + sealed.size());
            out->push_back(kUdpScreenAudio);
            out->push_back(flags);
            out->insert(out->end(), sealed.begin(), sealed.end());
            bytesTx_ += out->size();
            asio::post(io_, [this, out] { if (alive_) sendMedia(*out); });
        };
        int appGen = appGen_;
        while (sharing_ && gen == shareGen_) {
            auto now = std::chrono::steady_clock::now();
            if (appGen != appGen_) {  // the chosen application changed: reopen right away
                appGen = appGen_;
                cap.reset();
                warned = false;
                retryAt = now;
            }
            if (!shareAudio_ || !srvScreenAudio_ || viewers_ <= 0 || now < retryAt) {
                cap.reset();  // off or nobody watching: release the device
                acc.clear();
                frames = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (!cap) {
                std::string note;
                std::string app = shareAudioApp();
                cap = SystemAudioCapture::create(note, app);
                if (!cap) {
                    if (!warned) { say(T("! Screen audio: ") + note); warned = true; }
                    retryAt = now + std::chrono::seconds(3);
                    continue;
                }
                if (warned) say(T("* Screen audio: already being captured") + (app.empty() ? std::string() : " «" + app + "»"));
                warned = false;
                if (!cap->excludesSelf() && !warnedSelf) {
                    warnedSelf = true;
#ifdef _WIN32
                    say(T("! Your Windows can't keep Eco's sound out: viewers may hear their own voices. Use headphones "
                          "or Windows 10 (2004) or later."));
#else
                    say(T("! This system can't keep Eco's sound out of what you share: viewers may hear their own voices. "
                          "Use headphones."));
#endif
                }
            }
            if (!cap->read(acc)) {  // the output changed or was disconnected: reopen it
                cap.reset();
                retryAt = now + std::chrono::seconds(1);
                continue;
            }
            size_t off = 0;
            for (; acc.size() - off >= kStereoFrame; off += kStereoFrame) {
                const int16_t* f = acc.data() + off;
                bool silent = true;
                for (size_t i = 0; i < kStereoFrame && silent; ++i) silent = f[i] > -8 && f[i] < 8;
                quiet = silent ? quiet + 1 : 0;
                uint64_t ctr = screenAudioCtr_++;  // every frame has its own, also the ones that aren't sent
                if (quiet > 10) { flush(); continue; }  // 200 ms without sound: nothing is sent
                int n = opus_encode(enc, f, kFrameSamples, pkt, sizeof pkt);
                if (n <= 0) { flush(); continue; }
                if (frames == 0) {
                    first = ctr;
                    plain.assign(9, 0);
                    for (int i = 0; i < 8; ++i) plain[size_t(i)] = uint8_t(first >> (56 - 8 * i));
                }
                plain.push_back(uint8_t(n >> 8));
                plain.push_back(uint8_t(n));
                plain.insert(plain.end(), pkt, pkt + n);
                ++screenAudioTx_;
                if (++frames == kScreenAudioBatch) flush();
            }
            acc.erase(acc.begin(), acc.begin() + std::ptrdiff_t(off));
            if (acc.size() > kStereoFrame * 50) acc.clear();  // never more than 1 s piled up
        }
        opus_encoder_destroy(enc);
    }

    void stopSharing(bool announce) {
        if (!sharing_) return;
        sharing_ = false;  // the thread ends by itself; it is joined when sharing again or on exit
        if (announce) { sendTcp(Msg::ScreenStop); say(T("* You stopped sharing")); }
    }

    void startWatching(const User& u) {
        if (u.id == id_) return say(T("! That is your own screen"));
        if (!u.sharing) return say("! " + u.nick + T(" is not sharing their screen"));
        watching_ = u.id;
        canvas_ = Image{};
        audio_.clearScreenAudio();
        screenAudioSeen_ = ReplayWindow{};
        frameSeenAny_ = false;
        if (!screenSink_) {
            if (!viewer_) viewer_ = Viewer::create();
            viewer_->open(T("Eco - screen of ") + u.nick);
        }
        Writer w;
        w.u32(u.id);
        sendTcp(Msg::Watch, w.buf);
        say(T("* Opening the screen of ") + u.nick + (screenSink_ ? "" : T(" (/unwatch to close)")));
        publish();
    }

    void stopWatching() {
        if (!watching_) return;
        watching_ = 0;
        audio_.clearScreenAudio();
        Writer w;
        w.u32(0);
        sendTcp(Msg::Watch, w.buf);
        if (viewer_) {
            auto v = std::shared_ptr<Viewer>(std::move(viewer_));
            std::thread([v] { v->close(); }).detach();
        }
        say(T("* Viewer closed"));
        publish();
    }

    // ---------- User input ----------
    void cmd(const std::string& c) {
        Writer w;
        w.str(c);
        sendTcp(Msg::Command, w.buf);
    }

    void handleInput(const std::string& line) {
        handleInputImpl(line);
        publish();  // e.g. /e2e, /ptt or the password change what the GUI shows
    }

    void handleInputImpl(const std::string& line) {
        if (awaitingPass_) {  // the server asked for the account password
            awaitingPass_ = false;
            setEcho(true);
            pass_ = line;
            sendHello();
            return;
        }
        if (channel_.empty()) {  // connecting: it is kept and sent as soon as we are in (with the channel key)
            if (heldChat_.size() < 50) heldChat_.push_back(line);
            return;
        }
        if (line[0] != '/') {
            Writer w;
            if (line.size() > E2E::kMaxText) return say(T("! Message too long (max. 1200 characters)"));
            // Switching channel: we wait to be inside (and to have its key) to encrypt it; otherwise,
            // it would be bound to the previous channel and nobody in the new one could read it
            if (joinPending_) { heldChat_.push_back(line); return; }
            std::string sealed = e2e_.enabled() ? e2e_.sealText(line, adChat(nick_, channel_))
                                                : auto_.sealText(line, adChat(nick_, channel_));
            if (sealed.empty()) return say(T("! No encryption key ready yet; try again"));
            w.str(sealed);
            sendTcp(Msg::Chat, w.buf);
            return;
        }
        std::istringstream is(line.substr(1));
        std::vector<std::string> a;
        std::string t;
        while (is >> t) a.push_back(t);
        if (a.empty()) return;
        std::string c = canonicalCommand(lower(a[0]));
        auto rest = [&](size_t from) {
            std::string out;
            for (size_t i = from; i < a.size(); ++i) out += (i > from ? " " : "") + a[i];
            return out;
        };
        auto num = [&](size_t i, int def) {
            try { return a.size() > i ? std::stoi(a[i]) : def; } catch (...) { return def; }
        };

        if (c == "help" || c == "?") sayBlock(helpText());
        else if (c == "channels" || c == "c") {
            std::string out;
            for (auto& ch : channels_) {
                out += (ch == channel_ ? " > " : "   ") + ch + "\n";
                for (auto& [id, u] : users_)
                    if (u.channel == ch) out += "       " + decorate(u) + (id == id_ ? T(" (you)") : "") + "\n";
            }
            sayBlock(out);
        } else if (c == "join" || c == "j") {
            Writer w;
            w.str(rest(1));
            sendTcp(Msg::Join, w.buf);
            joinPending_ = lower(rest(1)) != lower(channel_);
        } else if (c == "pm" || c == "msg") {
            if (a.size() < 3) return say(T("! Usage: /pm <nick> <message>"));
            std::string text = rest(2);
            if (text.size() > E2E::kMaxText) return say(T("! Message too long (max. 1200 characters)"));
            if (e2e_.enabled()) {
                cmd("pm " + a[1] + " " + e2e_.sealText(text, adPm(nick_, a[1])));
            } else if (User* u = findUser(a[1])) {
                if (isBlocked(*u))
                    return say(T("! NOT sent: the key of ") + u->nick + T(" is not the one you verified. Check it with /verify ") +
                               u->nick + T(" and, if it is correct, /accept ") + u->nick);
                std::string sealed = auto_.sealPm(text, u->key, adPm(nick_, u->nick));
                if (sealed.empty())
                    return say(T("! Could not encrypt the private message for ") + u->nick +
                               T(" (their key is not valid or the encryption state could not be saved to disk)"));
                cmd("pm " + u->nick + " " + sealed);
            } else {
                // Not connected: their public key is requested from the server and it is sent when it arrives
                pendingPm_[lower(a[1])].push_back(text);
                cmd("key " + a[1]);
            }
        } else if (c == "verify") {
            if (a.size() < 2) return say(T("! Usage: /verify <nick>"));
            PubKey k;
            if (!keyOfNick(a[1], k)) return say(T("! I don't know the key of '") + a[1] + "'");
            say(T("* Safety code with ") + a[1] + ": " + auto_.safetyCode(k) +
                (auto_.isVerified(serverKey_, a[1], k) ? "  (VERIFICADO)" : ""));
            say(T("  Compare it with the one ") + a[1] + T(" sees (in person or another way). If it matches, nobody"));
            say(T("  can be reading or impersonating your conversation, not even the server."));
            if (auto_.blocked(serverKey_, a[1], k))
                say(T("  !!! It is DIFFERENT from the one you verified. If you checked it again and it is correct: /accept ") + a[1]);
            else if (!auto_.isVerified(serverKey_, a[1], k))
                say(T("  If it matches, type /confirm ") + a[1] + T(": if their key changes, Eco will block it until you check it."));
        } else if (c == "invite" || c == "invitacion" || c == "invitación") {
            std::string inv = inviteText();
            if (inv.empty()) return say(T("! The server's identity is not known yet"));
            say(T("* Invitation to this server (your friend pastes it into Eco when connecting; it checks the "
                  "server's identity by itself):"));
            say("  " + inv);
            if (!spass_.empty()) say(T("  It contains the server's password: only give it to people you trust."));
            if (host_ == "127.0.0.1" || host_ == "localhost" || host_ == "::1")
                say(T("  Careful: it uses 127.0.0.1, which only works on this PC. Change that part to your IP or domain."));
        } else if (c == "confirm" || c == "verificado") {
            if (a.size() < 2) return say(T("! Usage: /confirm <nick>  (after comparing the code with /verify)"));
            PubKey k;
            if (!keyOfNick(a[1], k)) return say(T("! I don't know the key of '") + a[1] + "'");
            if (auto_.blocked(serverKey_, a[1], k))
                return say(T("! Their key changed since you verified it: use /accept ") + a[1] + T(" if you have checked it"));
            auto_.setVerified(serverKey_, a[1], k);
            refreshTrust();
            say("* " + a[1] + T(" marked as verified. If their key changes, it will be blocked until you check it."));
        } else if (c == "accept") {
            if (a.size() < 2) return say(T("! Usage: /accept <nick>  (accepts their new key after checking it with /verify)"));
            PubKey k;
            if (!keyOfNick(a[1], k)) return say(T("! I don't know the key of '") + a[1] + "'");
            if (!auto_.blocked(serverKey_, a[1], k)) return say("* " + a[1] + T(" is not blocked"));
            auto_.setVerified(serverKey_, a[1], k);  // the new one becomes the verified one
            warnedBlocked_.erase(lower(a[1]));
            refreshTrust();
            say(T("* Accepted the new key of ") + a[1] + T(" (now marked as verified)."));
            shareKeys(true);  // now they do receive the channel key
        } else if (c == "e2e") {
            if (a.size() < 2) {
                say(e2e_.enabled() ? T("* E2E with phrase on. Check code: ") + e2e_.check()
                                   : T("* Automatic E2E on. Optional: /e2e <shared secret phrase> adds a group phrase"));
            } else if (a.size() == 2 && lower(a[1]) == "off") {
                e2e_.clear();
                warnedPlain_.clear();
                say(T("* Phrase disabled: back to automatic end-to-end encryption"));
            } else {
                enableE2E(rest(1));
            }
        } else if (c == "mute" || c == "m") {
            audio_.muted = !audio_.muted;
            say(audio_.muted ? T("* Microphone muted") : T("* Microphone on"));
        } else if (c == "deafen" || c == "deaf") {
            audio_.deafened = !audio_.deafened;
            audio_.muted = audio_.deafened.load();
            say(audio_.deafened ? T("* Sound off (and mic muted)") : T("* Sound on"));
        } else if (c == "ptt") {
            std::string key = a.size() > 1 ? lower(a[1]) : "";
            pttRun_ = false;
            if (pttT_.joinable()) pttT_.join();
            if (key.empty() || key == "off") {
                audio_.pushToTalk = false;
                say(T("* Push to talk off: voice detection"));
            } else {
                audio_.pushToTalk = true;
                pttKey_ = key;
                pttRun_ = true;
                pttT_ = std::thread([this, key] {
                    while (pttRun_) {
                        audio_.pttHeld = globalKeyDown(key);
                        std::this_thread::sleep_for(std::chrono::milliseconds(15));
                    }
                    audio_.pttHeld = false;
                });
                say(T("* Push to talk with the key '") + key + "'");
            }
        } else if (c == "threshold") {
            int v = std::max(0, std::min(100, num(1, 12)));
            audio_.vadThreshold = v / 1000.f;
            say(T("* Voice threshold: ") + std::to_string(v));
        } else if (c == "vol") {
            int v = std::max(0, std::min(200, num(1, 100)));
            audio_.volume = v / 100.f;
            say(T("* Volume: ") + std::to_string(v) + "%");
        } else if (c == "agc" || c == "autovolumen") {
            std::string v = a.size() > 1 ? lower(a[1]) : "";
            bool on = v.empty() ? !audio_.autoGain : (v == "on" || v == "si" || v == "sí" || v == "1" || v == "true");
            audio_.autoGain = on;
            audio_.saveChoice();
            say(on ? T("* Automatic mic volume on: your voice will arrive at a standard level")
                   : T("* Automatic mic volume off"));
        } else if (c == "volume") {
            if (a.size() < 3) return say(T("! Usage: /volume <nick> <0-300>   (100 = normal)"));
            int v = std::max(0, std::min(300, num(2, 100)));
            applyUserVolume(a[1], v / 100.f);
            say(T("* Volume of ") + a[1] + ": " + std::to_string(v) + "%");
        } else if (c == "noise") {
            std::string v = a.size() > 1 ? lower(a[1]) : "";
            bool on = v.empty() ? !audio_.noiseSuppression
                                : (v == "on" || v == "si" || v == "sí" || v == "1" || v == "true");
            audio_.noiseSuppression = on;
            audio_.saveChoice();
            say(on ? T("* Noise suppression on") : T("* Noise suppression off"));
        } else if (c == "quality") {
            int v = std::max(8, std::min(128, num(1, 40)));
            audio_.bitrate = v * 1000;
            say(T("* Voice quality: ") + std::to_string(v) + T(" kbps"));
        } else if (c == "sources") {
            sources_ = ScreenCapturer::sources();
            std::string out = T("-- What you can share (/share <fps> <width> <number>) --\n");
            for (size_t i = 0; i < sources_.size(); ++i) out += "  " + std::to_string(i + 1) + ") " + sources_[i].label + "\n";
            sayBlock(out);
        } else if (c == "share") {
            // Optional third argument: number from /sources or the id ("pantalla:1", "ventana:...")
            std::string src;
            if (a.size() > 3) {
                if (a[3].find(':') != std::string::npos) src = a[3];
                else {
                    if (sources_.empty()) sources_ = ScreenCapturer::sources();
                    int n = num(3, 0);
                    if (n < 1 || n > int(sources_.size())) return say(T("! There is no source ") + a[3] + T(": see /sources"));
                    src = sources_[size_t(n - 1)].id;
                }
            }
            startSharing(std::max(1, std::min(30, num(1, 5))), std::max(320, std::min(3840, num(2, 1280))), src);
        } else if ((c == "shareaudio") && a.size() > 1 && lower(a[1]) == "apps") {
            appList_ = SystemAudioCapture::apps();
            std::string out = T("-- Applications with sound (/shareaudio app <number> | all) --\n");
            if (appList_.empty()) out += T("  (none right now, or this system doesn't allow choosing)\n");
            for (size_t i = 0; i < appList_.size(); ++i) out += "  " + std::to_string(i + 1) + ") " + appList_[i].label + "\n";
            out += T("  Now: ") + (shareAudioApp().empty() ? std::string(T("the whole PC (except Eco)")) : shareAudioApp()) + "\n";
            sayBlock(out);
        } else if ((c == "shareaudio") && a.size() > 2 && lower(a[1]) == "app") {
            std::string v = lower(a[2]), pick;
            if (v != "todo" && v != "all") {
                if (appList_.empty()) appList_ = SystemAudioCapture::apps();
                int n = std::atoi(v.c_str());
                if (n >= 1 && n <= int(appList_.size())) pick = appList_[size_t(n - 1)].exe;
                else if (v.find('.') != std::string::npos) pick = v;  // a name: "chrome.exe"
                else return say(T("! There is no application ") + a[2] + T(": see /shareaudio apps"));
            }
            setShareAudioApp(pick);
            say(pick.empty() ? T("* Screen audio: everything playing on the PC (except Eco)")
                             : T("* Screen audio: only «") + pick + "»");
        } else if (c == "shareaudio") {
            std::string v = a.size() > 1 ? lower(a[1]) : "";
            bool on = v.empty() ? !shareAudio_ : (v == "on" || v == "si" || v == "sí" || v == "1" || v == "true");
            shareAudio_ = on;
            if (on && !srvScreenAudio_)
                say(T("! This server is from an older version and does not relay screen audio"));
            else
                say(on ? T("* When sharing the screen, the PC audio is sent too")
                       : T("* When sharing the screen, the PC audio is not sent"));
        } else if (c == "stopshare") {
            stopSharing(true);
        } else if (c == "watch") {
            if (a.size() < 2) {
                std::string l = T("* Sharing now:");
                for (auto& [id, u] : users_) if (u.sharing && u.channel == channel_) l += " " + u.nick;
                return say(l);
            }
            User* u = findUser(a[1]);
            if (!u) return say(T("! There is nobody called '") + a[1] + "'");
            startWatching(*u);
        } else if (c == "unwatch") {
            stopWatching();
        } else if (c == "devices" || c == "audio") {
#ifndef _WIN32
            reopenAudio(false);  // on Windows the watcher already keeps the list up to date
#endif
            sayBlock(T("Audio system: ") + audio_.hostApiName() + "\n" + audio_.describe(true) + audio_.describe(false) +
                     T("Change: /mic <number>  /output <number>  (or 'auto' to follow the Windows default)\n"));
            publishDevices();
        } else if (c == "mic" || c == "microfono" || c == "micrófono" || c == "entrada") {
            chooseDeviceCmd(true, a, rest(1));
        } else if (c == "output" || c == "altavoz" || c == "audifonos" || c == "audífonos" || c == "cascos") {
            chooseDeviceCmd(false, a, rest(1));
        } else if (c == "level") {
            int lvl = int(audio_.micLevel.load() * 1000);
            say(T("* Mic level now: ") + std::to_string(lvl) + T(" (threshold ") +
                std::to_string(int(audio_.vadThreshold.load() * 1000)) + T("). If it doesn't go over the threshold when you speak, lower /threshold"));
        } else if (c == "version" || c == "versión") {
            say(T("* Eco ") + std::string(kVersion) + T(" (protocol ") + std::to_string(kProtoVersion) + T("), server ") +
                (serverVersion_.empty() ? std::string(T("older than 2.1")) : serverVersion_));
        } else if (c == "language" || c == "lang") {
            i18n::Lang l;
            if (a.size() < 2 || !i18n::parse(a[1], l))
                return say(std::string(T("* Language: ")) + i18n::name(i18n::lang()) + T(". To change it: /language es | /language en"));
            i18n::set(l);
            i18n::save(l);
            say(T("* Language set to English"));
        } else if (c == "update") {
            beginUpdate();
        } else if (c == "ping") {
            say(T("* Ping: ") + (rttMs_ < 0 ? std::string(T("not measured yet")) : std::to_string(rttMs_.load()) + T(" ms")) +
                T(" | lost voice: ") + (lossPct_ < 0 ? std::string(T("no data (nobody has spoken)")) : fmtPct(lossPct_)) +
                (tcpVoice_ ? T(" | voice over TCP (UDP blocked)") : ""));
        } else if (c == "stats") {
            say(T("* Voice sent: ") + std::to_string(audio_.txPackets.load()) + T(" packets, received: ") +
                std::to_string(audio_.rxPackets.load()) + " | UDP " + (udpOk_ ? "OK" : T("no response (firewall?)")) +
                T(" | encryption: XChaCha20-Poly1305") + (e2e_.enabled() ? T(" + E2E with phrase (") + e2e_.check() + ")" : T(" + automatic E2E")) +
                T(" | TCP queue ") + std::to_string(pending_ / 1024) + T(" KB | screen sent ") +
                std::to_string(framesTx_.load()) + T(" frames (") + std::to_string(bytesTx_ / 1024) + T(" KB), received ") +
                std::to_string(framesRx_.load()) + " (" + std::to_string(bytesRx_ / 1024) + T(" KB) | screen audio sent ") +
                std::to_string(screenAudioTx_.load()) + T(", received ") + std::to_string(screenAudioRx_.load()));
        } else {
            // The commands the server handles, by the name the server knows them by
            static const std::map<std::string, std::string> alias = {
                {"kick", "kick"},         {"ban", "ban"},           {"unban", "unban"},       {"bans", "bans"},
                {"move", "move"},         {"silence", "mute"},      {"unsilence", "unmute"},  {"createchannel", "mkchan"},
                {"deletechannel", "rmchan"}, {"role", "role"},      {"login", "login"},       {"register", "register"},
                {"password", "passwd"},   {"history", "history"},   {"pms", "pmhistory"},     {"accounts", "accounts"},
                {"deleteaccount", "delaccount"}, {"resetpassword", "resetpass"}};
            auto it = alias.find(c);
            if (it == alias.end()) return say(T("! Unknown command. Type /help"));
            // The rest of the line is passed as is (a password can contain double spaces)
            size_t sp = line.find_first_of(" \t");
            cmd(it->second + (a.size() > 1 && sp != std::string::npos ? " " + line.substr(sp + 1) : ""));
        }
    }

    std::string host_;
    uint16_t port_;
    std::string nick_, spass_, pass_;
    asio::io_context io_;
    tcp::socket tcp_;
    udp::socket udp_;
    udp::endpoint srvUdp_, udpFrom_;
    asio::steady_timer pingTimer_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::thread net_, shareT_, shareAudioT_, pttT_;

    uint8_t hdr_[4];
    std::vector<uint8_t> body_;
    std::array<uint8_t, 2048> udpBuf_{};
    std::deque<std::shared_ptr<std::vector<uint8_t>>> outq_;
    std::atomic<size_t> pending_{0};

    std::atomic<bool> alive_{true}, down_{false}, sharing_{false}, keyReq_{false}, pttRun_{false}, udpOk_{false};
    std::atomic<int> viewers_{0};
    std::atomic<int> shareGen_{0};
    std::atomic<uint64_t> screenAudioCtr_{0};  // frame counter of the screen audio (never repeats)
    ReplayWindow screenAudioSeen_;             // counters already received from the screen being watched (network thread)
    std::atomic<bool> shareAudio_{false};      // send the PC audio while sharing
    std::atomic<bool> srvScreenAudio_{false};  // the server knows how to relay it
    std::string serverVersion_;                // the one the server reports ("" = older than 2.1)
    uint8_t srvFeatures_ = 0;                  // kFeat* the server announces
    std::atomic<bool> tcpVoice_{false};        // UDP blocked: voice over TCP
    // Diagnostics: ECO_SIN_UDP=1 acts as if the network blocked UDP (to test voice over TCP)
    const bool noUdp_ = [] { const char* v = std::getenv("ECO_SIN_UDP"); return v && *v == '1'; }();
    std::atomic<bool> retryable_{true};        // can reconnect by itself only if it drops
    std::atomic<int> rttMs_{-1};
    std::atomic<float> lossPct_{-1.f};
    uint64_t lastPlayed_ = 0, lastLost_ = 0;
    std::chrono::steady_clock::time_point welcomeAt_{}, udpPingAt_{}, lastPong_{};
    std::atomic<uint64_t> frameCtr_{1};        // counter of the frames I send
    uint64_t frameSeen_ = 0;                   // the last one that arrived from the screen I watch
    bool frameSeenAny_ = false;
    std::string rejoin_;                       // channel to go back to when reconnecting
    std::vector<CaptureSource> sources_;       // the last /sources list
    // Update offered by the server (already checked) and its download (network thread)
    update::Manifest upd_;
    bool updAvail_ = false;
    std::atomic<bool> updReady_{false};
    std::atomic<float> updProgress_{-1.f};
    struct Download {
        bool active = false;
        std::string dir;
        std::vector<size_t> todo;  // indices into upd_.files
        size_t pos = 0;
        uint64_t off = 0, done = 0, total = 0;
        std::ofstream out;
        crypto_blake2b_ctx hash{};
        std::chrono::steady_clock::time_point lastAt{};
    } dl_;
    std::vector<AudioApp> appList_;            // the last /shareaudio apps list
    mutable std::mutex appMu_;
    std::string appChoice_;                    // which application the screen audio comes from ("" = everything)
    std::atomic<int> appGen_{0};
    std::atomic<uint64_t> framesTx_{0}, framesRx_{0}, bytesTx_{0}, bytesRx_{0}, screenAudioTx_{0}, screenAudioRx_{0};
    uint32_t id_ = 0, watching_ = 0;

    // Cifrado
    SessionKeys keys_;
    TcpCipher tx_, rx_;
    uint64_t udpTx_ = 0;
    ReplayWindow udpRx_;
    uint8_t serverId_[32] = {0};
    E2E e2e_;    // E2E with a shared phrase (optional)
    Keys auto_;  // Automatic E2E with per-device keys
    std::string serverKey_;
    std::string expectedFp_, serverFp_;  // invitation fingerprint / the one the server presents (hex)
    bool serverVerified_ = false;        // coincidieron
    std::vector<std::pair<uint32_t, PubKey>> members_;
    std::map<std::string, std::vector<std::string>> pendingPm_;
    bool joinPending_ = false;           // a channel change was requested and the confirmation hasn't arrived yet
    std::vector<std::string> heldChat_;  // what was typed in the meantime
    void flushHeldChat() {
        joinPending_ = false;
        auto held = std::move(heldChat_);
        heldChat_.clear();
        for (auto& l : held) handleInput(l);
    }
    std::vector<uint8_t> udpPlain_, voicePlain_, screenPlain_, udpSeal_;
    std::set<uint32_t> warnedE2E_, warnedPlain_;
    std::set<std::string> warnedBlocked_;
    bool awaitingPass_ = false;
    std::atomic<uint16_t> seq_{0};
    int pingCount_ = 0;
    Role role_ = Role::User;
    std::string channel_;
    std::vector<std::string> channels_;
    std::map<uint32_t, User> users_;
    bool registered_ = false;
    std::string authMsg_, pttKey_;
    int authSeq_ = 0;

    // What the interface reads from its thread
    mutable std::mutex stateMu_;
    UiState snap_;
    std::map<uint32_t, std::chrono::steady_clock::time_point> lastVoice_;
    std::string error_, newHostKey_;

    Audio audio_;
    std::unique_ptr<Viewer> viewer_;
    ScreenDecoder decoder_;
    Image canvas_;
};

}  // namespace

namespace eco {

std::unique_ptr<EcoClient> EcoClient::create(const ClientOptions& o) { return std::make_unique<Client>(o); }

void setLogSink(LogSink sink) {
    std::lock_guard<std::mutex> l(printMu);
    g_sink = std::move(sink);
}

bool parseHostPort(const std::string& in, std::string& host, uint16_t& port) {
    host = in;
    port = kDefaultPort;
    while (!host.empty() && host.back() == ' ') host.pop_back();
    while (!host.empty() && host.front() == ' ') host.erase(host.begin());
    try {
        // host:port  (and [ipv6]:port)
        if (host.size() && host[0] == '[') {
            auto e = host.find(']');
            if (e != std::string::npos) {
                if (e + 2 < host.size() && host[e + 1] == ':') {
                    int p = std::stoi(host.substr(e + 2));
                    if (p <= 0 || p > 65535) return false;
                    port = uint16_t(p);
                }
                host = host.substr(1, e - 1);
            }
        } else if (std::count(host.begin(), host.end(), ':') == 1) {
            auto c = host.find(':');
            int p = std::stoi(host.substr(c + 1));
            if (p <= 0 || p > 65535) return false;
            port = uint16_t(p);
            host = host.substr(0, c);
        }
    } catch (const std::exception&) {
        return false;
    }
    return !host.empty();
}

}  // namespace eco

int clientMain(int argc, char** argv) {
    eco::update::cleanupOld();  // what a previous update left behind
    i18n::set(i18n::initial(true));
    std::vector<std::string> pos;
    ClientOptions o;
    bool help = false;
    // Secrets through environment variables: on the command line anyone could see them with "ps"/Task
    // Manager. Explicit options take precedence.
    if (const char* v = std::getenv("ECO_CLAVE")) o.accountPass = v;
    if (const char* v = std::getenv("ECO_PASS")) o.serverPass = v;
    if (const char* v = std::getenv("ECO_E2E")) o.e2ePhrase = v;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--pass") o.serverPass = next();
        else if (a == "--idioma" || a == "--lang" || a == "--language") {
            i18n::Lang l;
            if (i18n::parse(next(), l)) i18n::set(l);
        }
        else if (a == "--clave" || a == "--password") o.accountPass = next();
        else if (a == "--e2e") o.e2ePhrase = next();
        else if (a == "--confiar" || a == "--trust") o.trustNewKey = true;
        else if (a == "--entrada" || a == "--input") o.input = next();
        else if (a == "--salida" || a == "--output") o.output = next();
        else if (a == "--tono" || a == "--tone") o.testTone = true;
        else if (a == "--dispositivos" || a == "--devices") { std::cout << Audio::listDevicesStandalone(); return 0; }
        else if (a == "-h" || a == "--help" || a == "--ayuda") help = true;
        else if (a == "-v" || a == "--version") { std::cout << T("eco-client ") << kVersion << T(" (protocol ") << kProtoVersion << ")\n"; return 0; }
        else pos.push_back(a);
    }
    if (help || pos.size() != 2) {
        std::cout << T("Eco ") << kVersion << T(" - lightweight encrypted voice chat\nUsage: eco-client <server[:port] | eco://... invitation> "
                                                "<nick> [--password your-password] [--pass server-password]\n                 [--e2e \"shared "
                                                "phrase\"] [--trust] [--input N] [--output N]\nOther:       eco-client --devices  |  eco-client "
                                                "--version  |  --language es|en\nPasswords without leaving them visible in the process list: "
                                                "ECO_CLAVE, ECO_PASS and ECO_E2E\nExample:     eco-client 192.168.1.10 John\nThe server is a "
                                                "separate program: eco-server\n");
        return help ? 0 : 1;
    }
    if (isInvite(pos[0])) {
        Invite inv;
        if (!parseInvite(pos[0], inv)) {
            std::cout << T("The invitation is not valid: copy all of it, starting with eco://\n");
            return 1;
        }
        o.host = inv.host;
        o.port = inv.port;
        o.expectedFingerprint = inv.fingerprint;
        if (o.serverPass.empty()) o.serverPass = inv.serverPass;
    } else if (!parseHostPort(pos[0], o.host, o.port)) {
        std::cout << T("Invalid port in '") << pos[0] << "'\n";
        return 1;
    }
    o.nick = pos[1];
    Client client(o);
    if (!client.connect()) return 1;
    client.run();
    return 0;
}
