// Eco - protocol shared between server and client.
// TCP: control, chat, permissions and screen.  UDP: voice (Opus).
// Everything is encrypted (see secure.hpp) except the initial key exchange greeting.
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace eco {

constexpr uint16_t kDefaultPort = 9987;
constexpr uint32_t kMaxFrame = 8u * 1024u * 1024u;  // 8 MB per TCP message
constexpr uint32_t kProtoVersion = 4;  // 4: Double Ratchet in private messages and signatures in channels

// Audio: 48 kHz mono, 20 ms frames
constexpr int kSampleRate = 48000;
constexpr int kFrameSamples = 960;

enum class Msg : uint8_t {
    // Cliente -> Servidor
    Hello = 1,        // str nick, str accountPassword, str serverPassword, 32 bytes E2E public key, str version (may be missing)
    Join = 2,         // str canal
    Chat = 3,         // str texto
    Command = 4,      // str "kick juan"
    ScreenStart = 5,  //
    ScreenStop = 6,   //
    ScreenFrame = 7,  // u8 flags, datos (ver abajo)
    Watch = 8,        // u32 userId (0 = stop watching)
    Ping = 9,         // (u64 stamp: if present, the server answers Pong with it)
    KeyShare = 10,    // u16 n, per recipient: u32 id, bytes encryptedSenderKey
    VoiceTcp = 12,    // the same as a plaintext UDP packet ('K', 'E' or 'A' ...), when UDP is blocked
    TcpVoiceMode = 13,  // u8 1/0: send me the voice over TCP (my UDP doesn't work)
    UpdateQuery = 14,   // str platform ("windows-x64"): is there a published version?
    UpdateGet = 15,     // str platform, str file, u32 from: a piece of an update file

    // Servidor -> Cliente
    Welcome = 100,     // u32 id, u16 udpPort, u8 role, u8 registered, u8 features (kFeat*), str version (may be missing)
    Error = 101,       // str texto
    Info = 102,        // str texto
    ChatMsg = 103,     // str channel, str from, str text, u8 private (2 = copy of your private message; channel = recipient)
    State = 104,       // list of channels and users
    ScreenFrom = 105,  // u32 idOrigen, u8 flags, datos
    KeyRequest = 106,  // the server asks for a full frame
    Kicked = 107,      // str motivo
    Role = 108,        // u8 rol nuevo
    Viewers = 109,     // u16 how many are watching your screen
    AuthFail = 110,    // str text: the client asks for the password and resends Hello
    KeyFrom = 112,     // u32 idOrigen, bytes claveDeEmisorCifrada
    PeerKey = 113,     // str nick, u8 exists, 32 bytes public key
    VoiceFrom = 114,   // the same as a plaintext UDP packet from the server (voice or screen audio) over TCP
    Pong = 115,        // u64 the Ping stamp
    UpdateInfo = 116,  // str plataforma, bytes manifiesto, bytes firma (ver update.hpp)
    UpdateChunk = 117, // str file, u32 from, u32 total, bytes data
    History = 111,     // str title, str channel, u16 n, per message: u64 date, str from, str to, str text
};

enum class Role : uint8_t { User = 0, Mod = 1, Admin = 2 };

// For display (pass it through T()). roleName() is the one that goes inside server messages, in Spanish.
inline const char* roleLabel(Role r) {
    switch (r) {
        case Role::Admin: return "admin";
        case Role::Mod: return "mod";
        default: return "user";
    }
}

inline const char* roleName(Role r) {
    switch (r) {
        case Role::Admin: return "admin";
        case Role::Mod: return "mod";
        default: return "usuario";
    }
}

// ---- Minimal serialization (big endian) ----
class Writer {
public:
    std::vector<uint8_t> buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) { buf.push_back(uint8_t(v >> 8)); buf.push_back(uint8_t(v)); }
    void u32(uint32_t v) { for (int s = 24; s >= 0; s -= 8) buf.push_back(uint8_t(v >> s)); }
    void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    void str(const std::string& s) {
        uint16_t n = uint16_t(s.size() > 65535 ? 65535 : s.size());
        u16(n);
        buf.insert(buf.end(), s.begin(), s.begin() + n);
    }
    void bytes(const uint8_t* p, size_t n) { u32(uint32_t(n)); buf.insert(buf.end(), p, p + n); }
    void raw(const uint8_t* p, size_t n) { buf.insert(buf.end(), p, p + n); }
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    uint8_t u8() { need(1); return *p_++; }
    uint16_t u16() { need(2); uint16_t v = uint16_t(p_[0] << 8 | p_[1]); p_ += 2; return v; }
    uint32_t u32() {
        need(4);
        uint32_t v = uint32_t(p_[0]) << 24 | uint32_t(p_[1]) << 16 | uint32_t(p_[2]) << 8 | p_[3];
        p_ += 4;
        return v;
    }
    uint64_t u64() { uint64_t h = u32(); return h << 32 | u32(); }
    std::string str() { uint16_t n = u16(); need(n); std::string s((const char*)p_, n); p_ += n; return s; }
    std::vector<uint8_t> bytes() { uint32_t n = u32(); need(n); std::vector<uint8_t> v(p_, p_ + n); p_ += n; return v; }
    // Like bytes(), without copying: points inside the message (valid while the buffer lives)
    const uint8_t* bytesView(size_t& n) { n = u32(); need(n); const uint8_t* v = p_; p_ += n; return v; }
    const uint8_t* bytesView16(size_t& n) { n = u16(); need(n); const uint8_t* v = p_; p_ += n; return v; }
    const uint8_t* ptr() const { return p_; }
    size_t left() const { return size_t(end_ - p_); }
    void skip(size_t n) { need(n); p_ += n; }

private:
    void need(size_t n) { if (size_t(end_ - p_) < n) throw std::runtime_error("mensaje truncado"); }
    const uint8_t* p_;
    const uint8_t* end_;
};

// Plaintext body of a message: [u8 type][payload]. Then it is encrypted (secure.hpp).
inline std::vector<uint8_t> body(Msg type, const std::vector<uint8_t>& payload = {}) {
    std::vector<uint8_t> out;
    out.reserve(1 + payload.size());
    out.push_back(uint8_t(type));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// ---- Voice UDP (encrypted) ----
// Cliente->Servidor: u32 idSesion, u64 contador, cifrado{ 'V' u16 seq, opus... | 'P' }, mac16
// Servidor->Cliente: u64 contador, cifrado{ 'V' u32 idOrigen, u16 seq, opus... | 'P' }, mac16
constexpr uint8_t kUdpVoice = 'V';
constexpr uint8_t kUdpPing = 'P';
constexpr uint8_t kUdpVoiceE2E = 'E';   // like 'V', but the opus is end-to-end encrypted with a phrase
constexpr uint8_t kUdpVoiceAuto = 'K';  // same, with the sender key (automatic E2E)
// PC audio while sharing the screen. It goes over UDP, like voice: over TCP it would wait behind the frames.
// Cliente->Servidor: 'A' u8 flags, blob   Servidor->Cliente: 'A' u32 idOrigen, u8 flags, blob
// Only those watching that screen receive it. The blob is encrypted and signed like a frame (see below).
constexpr uint8_t kUdpScreenAudio = 'A';
constexpr size_t kMaxUdpPayload = 1300;  // the most the server relays (not counting headers)

// ---- Pantalla compartida ----
// ScreenFrame payload: u8 flags, datos
//   without E2E: data = frame;  with E2E: data = encrypted frame (the server can't see it)
// Fotograma: u16 ancho, u16 alto, u8 esClave, u16 nTeselas,
//   per tile: u16 x, u16 y, u16 w, u16 h, bytes jpeg
constexpr uint8_t kScreenKey = 1;
constexpr uint8_t kScreenE2E = 2;   // encrypted with a phrase
constexpr uint8_t kScreenAuto = 4;  // encrypted with the sender key
constexpr uint8_t kScreenSigned = 8;  // also signed (EdDSA of the sender key)
// The plaintext frame carries at the end a u64 counter that never repeats (anti-replay). Clients
// older than 2.2 ignore what is left over at the end of the frame, so they keep seeing it.
constexpr uint8_t kScreenCounter = 32;
constexpr uint8_t kScreenAudio = 16;  // it is PC audio, not a frame (it also goes in the authenticated data)
constexpr int kTile = 128;

// Screen audio: same encryption as the frames, with kScreenAudio in flags.
// In plaintext: u64 counter (of the first frame; goes up 1 per 20 ms frame and never repeats), u8 n (1..3),
//   n times: u16 length, opus (48 kHz stereo, 20 ms). The counter allows dropping repeated packets.
constexpr int kScreenAudioBatch = 3;        // frames per packet (60 ms): fewer signatures and headers
constexpr int kScreenAudioMaxOpus = 330;    // bytes per frame at most (so the packet fits in kMaxUdpPayload)

// Features the server announces in Welcome (old servers don't send that byte)
constexpr uint8_t kFeatScreenAudio = 1;
constexpr uint8_t kFeatTcpVoice = 2;   // relays voice over TCP to whoever has no UDP (VoiceTcp/TcpVoiceMode)
constexpr uint8_t kFeatUpdates = 4;    // sirve actualizaciones firmadas (UpdateQuery/UpdateGet)
constexpr uint8_t kFeatPong = 8;       // answers Ping with Pong (to measure the ping over TCP)

// State: flags of each user
constexpr uint8_t kUserSharing = 1, kUserMuted = 2, kUserRegistered = 4, kUserScreenAudio = 8;

}  // namespace eco
