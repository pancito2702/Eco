// Audio engine: capture -> high-pass filter -> noise suppression (RNNoise, optional) -> voice detector -> Opus -> network,
// and network -> jitter buffer (with Opus FEC and loss concealment) -> mix -> speakers.
//
// Devices: by default ("auto") the system's default microphone and output are used
// (on Windows, the ones Windows marks: if you plug in headphones and they become the
// default, Eco switches to them by itself). A specific one can also be pinned by name.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;
struct DenoiseState;

namespace eco {

struct AudioDevice {
    int pa = -1;         // PortAudio index (changes when scanning again)
    std::string name;
    bool isDefault = false;
};

class Audio {
public:
    using SendFn = std::function<void(const uint8_t* opus, size_t n)>;

    Audio();
    ~Audio();

    // All control (start/stop/restart/devices/select) must be done from the same thread.
    bool start(SendFn send);
    void stop();
    void shutdown();  // stop + liberar PortAudio
    // Scans the devices again (detects newly connected ones) and reopens the audio
    bool restart();

    // Device lists of the preferred API (WASAPI on Windows)
    std::vector<AudioDevice> inputs();
    std::vector<AudioDevice> outputs();
    // Chosen device: "" = system default. Searched by exact name and otherwise by part of the name.
    std::string inputChoice, outputChoice;
    // Name of the device open right now ("" if none)
    std::string inputName() const;
    std::string outputName() const;
    std::string hostApiName();
    std::vector<std::string> notes;  // warnings from the last attempt to open the audio

    // Text for the console
    std::string describe(bool input);
    static std::string listDevicesStandalone();

    // Remember the choice between sessions (devices and noise suppression)
    void loadChoice();
    void saveChoice() const;

    // Signals (from another thread) when the connected or default devices change
    std::function<void()> onDevicesChanged;

    // Called from the network thread when voice arrives
    void onPacket(uint32_t sender, uint16_t seq, const uint8_t* data, size_t n);
    void removeSender(uint32_t sender);
    // Volume of a specific person (1 = normal, up to 3 = 300 %)
    void setSenderGain(uint32_t sender, float gain);
    // Per-nick volume remembered between sessions (key: lowercase nick). Only from the control thread.
    std::map<std::string, float> nickVolume;
    void clearSenders();
    // PC audio of whoever shares the screen you are watching (stereo; network thread)
    void onScreenAudio(uint32_t sender, uint16_t seq, const uint8_t* data, size_t n);
    void clearScreenAudio();
    std::atomic<float> screenVolume{1.0f};  // volume of that audio (0..2), separate from the voices

    std::atomic<bool> muted{false};
    std::atomic<bool> deafened{false};
    std::atomic<bool> pushToTalk{false};  // if enabled, only transmits with pttHeld
    std::atomic<bool> pttHeld{false};
    std::atomic<float> vadThreshold{0.012f};  // RMS (0..1) to activate the mic
    std::atomic<float> volume{1.0f};
    std::atomic<bool> talking{false};
    std::atomic<int> bitrate{40000};
    std::atomic<bool> noiseSuppression{true};  // remove background noise from the mic (RNNoise)
    std::atomic<bool> autoGain{true};          // raise/lower only your voice to a standard level
    std::atomic<float> agcGain{1.f};           // gain the automatic control applies right now
    std::atomic<bool> testTone{false};       // sends a tone instead of the mic (tests)
    std::atomic<uint64_t> rxPackets{0};
    std::atomic<uint64_t> txPackets{0};
    std::atomic<uint64_t> playedFrames{0}, lostFrames{0};  // voice: frames that arrived / that were missing mid-sentence
    std::atomic<float> micLevel{0.f};        // RMS of the last mic frame (0..1)

private:
    bool ensureInit();
    bool openStreams();
    void closeStreams();
    int preferredHostApi();
    std::vector<AudioDevice> list(bool input);
    int resolve(bool input, const std::string& choice, bool& fellBack);
    void* openStream(bool input, int dev, int channels = 1);
    void captureLoop();
    void playbackLoop();
    void watchLoop();

    // Each person talking: their Opus packets by sequence number, decoded on playback
    struct Stream {
        OpusDecoder* dec = nullptr;
        std::map<int64_t, std::vector<uint8_t>> pkts;
        int64_t last = 0;     // highest sequence received (without the 16-bit wraparound)
        int64_t next = 0;     // the one to play next
        bool started = false;
        bool primed = false;  // playing (the initial cushion was gathered)
        int target = 2;       // desired cushion in 20 ms frames; grows if the network is choppy
        int plcRun = 0;       // consecutive frames made up for lack of a packet
        int calm = 0;         // frames without late packets (to lower the cushion again)
        int idle = 0;
        int ch = 1;                    // 2 = stereo (screen audio)
        int minTarget = 2, maxTarget = 6, maxPkts = 15;  // cushion: minimum, maximum when choppy, absolute cap
    };
    void push(Stream& s, uint16_t seq, const uint8_t* data, size_t n);
    bool decodeNext(Stream& s, int16_t* out);

    void* in_ = nullptr;
    void* out_ = nullptr;
    OpusEncoder* enc_ = nullptr;
    DenoiseState* dn_ = nullptr;
    SendFn send_;
    std::thread capT_, playT_, watchT_;
    std::atomic<bool> running_{false};
    std::atomic<bool> outOpen_{false};
    std::mutex mu_;
    std::map<uint32_t, Stream> streams_;
    std::map<uint32_t, float> senderGain_;  // protected by mu_
    Stream screen_;                // audio of the screen being watched (protected by mu_)
    uint32_t screenFrom_ = 0;
    int outCh_ = 1;                // channels of the open output (2 if it supports stereo)
    bool paInit_ = false;
    bool started_ = false;
    mutable std::mutex nameMu_;
    std::string inName_, outName_;

    std::mutex watchMu_;
    std::condition_variable watchCv_;
    bool watchStop_ = false;
};

}  // namespace eco
