// Interface of the Eco client for whoever controls it: the console (client.cpp) or the GUI (gui.cpp).
// The client runs its network on its own thread; everything here is safe from the interface thread.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../common/protocol.hpp"

namespace eco {

struct Image;  // screen.hpp

struct ClientOptions {
    std::string host;
    uint16_t port = kDefaultPort;
    std::string nick;
    std::string serverPass;   // --pass: password to enter the server
    std::string accountPass;  // --password: account password
    std::string e2ePhrase;    // --e2e
    std::string input, output;  // --entrada / --salida
    bool trustNewKey = false;   // --confiar
    bool testTone = false;
    // Expected server fingerprint (32 hex digits, from an invitation). If given, the server identity
    // has to match or it doesn't connect: the first connection is verified (no "blind trust")
    std::string expectedFingerprint;
    // If given, the screen being watched arrives here (network thread) instead of opening in a separate window
    std::function<void(const Image&)> onScreenFrame;
    std::string rejoinChannel;  // when reconnecting: go back to this channel
};

struct UiUser {
    uint32_t id = 0;
    std::string nick, channel;
    Role role = Role::User;
    bool sharing = false, muted = false, registered = false;
    bool screenAudio = false;  // their shared screen carries the PC audio
    bool talking = false;  // they were heard very recently
    bool me = false;
    float volume = 1.f;  // volume you gave them (1 = 100 %)
    bool verified = false;  // you compared the safety code and their key hasn't changed
    bool blocked = false;   // was verified and their key changed: blocked until checked
    std::string safetyCode;  // 30 digits to compare with this person
};

// Snapshot of the client state to draw it
struct UiState {
    bool alive = false;    // connected (or connecting)
    bool joined = false;   // already inside a channel
    std::string nick, channel;
    Role role = Role::User;
    bool registered = false;
    std::vector<std::string> channels;
    std::vector<UiUser> users;
    std::string authPrompt;  // if not empty, the server asks for the account password
    int authSeq = 0;         // goes up every time it asks (to ask again if it fails)
    // Audio
    bool muted = false, deafened = false, talking = false, pushToTalk = false;
    bool noiseSuppression = true;
    bool autoGain = true;
    float agcGain = 1.f;  // how much the automatic volume is raising (or lowering) your voice
    std::string pttKey;
    float micLevel = 0, threshold = 0.012f, volume = 1.f;
    int bitrateKbps = 40;
    std::string inputName, outputName, inputChoice, outputChoice;
    std::vector<std::string> inputs, outputs;  // tras requestDevices()
    // Pantalla
    bool sharing = false;
    bool shareAudio = false;  // when sharing, also send the PC audio
    std::string shareAudioApp;  // "" = the whole PC (except Eco); "chrome.exe" = only that application
    int viewers = 0;
    uint32_t watching = 0;
    // Network and encryption
    bool udpOk = false;
    bool voiceTcp = false;   // UDP blocked: voice goes over TCP
    int pingMs = -1;         // round trip to the server (-1 = not known yet)
    float lossPct = -1;      // voice lost in the last few seconds, in % (-1 = no voice has arrived)
    // Updates (signed; offered by the server)
    std::string updateVersion;   // there is a new version available ("" = no)
    float updateProgress = -1;   // downloading: 0..1 (-1 = no)
    bool updateReady = false;    // instalada: falta reiniciar
    bool e2ePhrase = false;
    std::string e2eCheck;
    std::string invite;            // invitation to this server to pass to a friend (eco://...)
    std::string serverVersion;     // server's Eco version ("" = older than 2.1)
    bool inviteHasPass = false;    // the invitation includes the server password
    bool serverVerified = false;   // the server identity was checked with an invitation
};

class EcoClient {
public:
    static std::unique_ptr<EcoClient> create(const ClientOptions& o);
    virtual ~EcoClient() = default;

    // Blocks until connected and the server is verified. If it fails, see error() and hostKeyChanged().
    virtual bool connect() = 0;
    virtual void shutdown() = 0;
    virtual bool alive() const = 0;
    virtual std::string error() const = 0;
    // If not empty, the server identity changed: it is the new fingerprint (connect with trustNewKey)
    virtual std::string hostKeyChanged() const = 0;

    // Text as in the console: message to the channel or /command
    virtual void submit(const std::string& line) = 0;
    virtual UiState state() = 0;

    // Direct controls (without going through commands, for sliders and buttons)
    virtual void setMuted(bool on) = 0;
    virtual void setDeafened(bool on) = 0;
    virtual void setVolume(float v) = 0;     // 0..2
    virtual void setThreshold(float v) = 0;  // 0..0.1
    virtual void setBitrate(int kbps) = 0;
    virtual void setNoiseSuppression(bool on) = 0;  // remembered for next time
    virtual void setAutoGain(bool on) = 0;          // automatic mic volume
    virtual void setUserVolume(const std::string& nick, float v) = 0;  // 0..3, remembered by nick
    virtual void setPushToTalk(const std::string& key) = 0;  // "" = desactivar
    virtual void requestDevices() = 0;  // fills UiState::inputs/outputs shortly
    virtual void chooseDevice(bool input, const std::string& name) = 0;  // "" = automatic
    virtual void setShareAudio(bool on) = 0;
    virtual void startUpdate() = 0;  // download and install the new version (if there is one)
    virtual void setShareAudioApp(const std::string& exe) = 0;  // "" = the whole PC; applied on the fly
    virtual void setScreenVolume(float v) = 0;  // volume of the audio of the screen you watch (0..2)
    // Is it worth reconnecting by itself if it drops? Not if you were kicked or the server identity changed
    virtual bool retryable() const = 0;  // share the PC audio with the screen (can be changed on the fly)
};

// Where the text that used to go to the console goes. One line per call (it may contain line breaks).
using LogSink = std::function<void(const std::string& line)>;
void setLogSink(LogSink sink);

// "host", "host:port" or "[ipv6]:port". Returns false if the port is not valid.
bool parseHostPort(const std::string& in, std::string& host, uint16_t& port);

}  // namespace eco

#include "../common/invite.hpp"  // invitaciones eco://

namespace eco {

}  // namespace eco
