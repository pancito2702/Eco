#include "audio.hpp"
#include "../common/i18n.hpp"

#include <opus/opus.h>
#include <portaudio.h>
#include <rnnoise.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "../common/protocol.hpp"
#include "keys.hpp"  // configPath

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <pa_win_wasapi.h>
#endif

namespace eco {

namespace {
constexpr int kMaxPacket = 1275;
constexpr int kJitterStart = 2;      // initial cushion: 2 frames (40 ms)
constexpr int kJitterTargetMax = 6;  // if the network is choppy the cushion grows up to 120 ms...
constexpr int kJitterMax = 15;       // ...and never more than 300 ms pile up
constexpr int kHangover = 20;        // 400 ms of "tail" after you stop talking (doesn't cut the end of sentences)
constexpr int kPreroll = 2;          // 40 ms from before the voice is detected (doesn't cut the beginning)
constexpr int kDenoiseFrame = 480;   // RNNoise works with 10 ms frames
// Automatic gain control: brings the voice to ~-20 dBFS (standard voice level in calls)
constexpr float kAgcTarget = 0.1f;
constexpr float kAgcMax = 6.f;   // at most +15.6 dB (very quiet mics)
constexpr float kAgcMin = 0.5f;  // at least -6 dB (mics that clip)

// So audio doesn't stutter when the PC is busy (e.g. gaming while talking)
void raiseThreadPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
}

// Soft limiter: with several people at once the sum doesn't "break" (no harsh clipping)
inline int16_t softClip(float x) {
    const float knee = 26000.f, room = 32767.f - knee;
    float a = std::fabs(x);
    if (a > knee) a = knee + room * (a - knee) / (a - knee + room);
    return int16_t(x < 0 ? -a : a);
}

std::string lowerStr(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}
}  // namespace

Audio::Audio() = default;

Audio::~Audio() { shutdown(); }

bool Audio::ensureInit() {
    if (!paInit_) paInit_ = Pa_Initialize() == paNoError;
    return paInit_;
}

// On Windows WASAPI is preferred: it reflects Windows' current default devices,
// has full names and doesn't repeat each device for every API (MME, DirectSound...).
int Audio::preferredHostApi() {
#ifdef _WIN32
    PaHostApiIndex w = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
    if (w >= 0) {
        const PaHostApiInfo* h = Pa_GetHostApiInfo(w);
        if (h && h->deviceCount > 0) return w;
    }
#endif
    return Pa_GetDefaultHostApi();
}

std::string Audio::hostApiName() {
    if (!ensureInit()) return "";
    const PaHostApiInfo* h = Pa_GetHostApiInfo(preferredHostApi());
    return h ? h->name : "";
}

std::vector<AudioDevice> Audio::list(bool input) {
    std::vector<AudioDevice> v;
    if (!ensureInit()) return v;
    int api = preferredHostApi();
    const PaHostApiInfo* h = Pa_GetHostApiInfo(api);
    if (!h) return v;
    int def = input ? h->defaultInputDevice : h->defaultOutputDevice;
    for (int i = 0; i < h->deviceCount; ++i) {
        int dev = Pa_HostApiDeviceIndexToDeviceIndex(api, i);
        const PaDeviceInfo* d = Pa_GetDeviceInfo(dev);
        if (!d || (input ? d->maxInputChannels : d->maxOutputChannels) <= 0) continue;
        v.push_back({dev, d->name, dev == def});
    }
    return v;
}

std::vector<AudioDevice> Audio::inputs() { return list(true); }
std::vector<AudioDevice> Audio::outputs() { return list(false); }

// "Stereo Mix" / "Mezcla estéreo" and the like record what plays on the PC, not your voice
static bool isLoopback(const std::string& name) {
    std::string n = lowerStr(name);
    for (const char* k : {"mezcla est", "stereo mix", "what u hear", "wave out mix", "loopback", "mix estéreo"})
        if (n.find(k) != std::string::npos) return true;
    return false;
}

// Part of the name that identifies the device: "Microphone (CORSAIR VOID...)" -> "(corsair void...)"
static std::string hardwareOf(const std::string& name) {
    auto p = name.rfind('(');
    return p == std::string::npos ? "" : lowerStr(name.substr(p));
}

int Audio::resolve(bool input, const std::string& choice, bool& fellBack) {
    fellBack = false;
    auto v = list(input);
    if (v.empty()) return paNoDevice;
    auto def = [&] {
        if (!input) {
            for (auto& d : v) if (d.isDefault) return d.pa;
            return v[0].pa;
        }
        // Automatic microphone:
        // 1) the one from the same headphones/headset that are the default output
        std::string hw;
        for (auto& d : list(false)) if (d.isDefault) hw = hardwareOf(d.name);
        if (!hw.empty())
            for (auto& d : v) if (hardwareOf(d.name) == hw && !isLoopback(d.name)) return d.pa;
        // 2) the Windows default, unless it is "Stereo Mix"
        for (auto& d : v) if (d.isDefault && !isLoopback(d.name)) return d.pa;
        // 3) the first real microphone
        for (auto& d : v) if (!isLoopback(d.name)) return d.pa;
        return v[0].pa;
    };
    if (choice.empty()) return def();
    for (auto& d : v) if (d.name == choice) return d.pa;
    std::string c = lowerStr(choice);
    for (auto& d : v) if (lowerStr(d.name).find(c) != std::string::npos) return d.pa;
    fellBack = true;
    return def();
}

std::string Audio::inputName() const { std::lock_guard<std::mutex> l(nameMu_); return inName_; }
std::string Audio::outputName() const { std::lock_guard<std::mutex> l(nameMu_); return outName_; }

void* Audio::openStream(bool input, int dev, int channels) {
    if (dev == paNoDevice) return nullptr;
    const PaDeviceInfo* info = Pa_GetDeviceInfo(dev);
    if (!info) return nullptr;
    PaStreamParameters p{};
    p.device = dev;
    p.channelCount = channels;
    p.sampleFormat = paInt16;
    double lat = input ? info->defaultLowInputLatency : info->defaultLowOutputLatency;
    // A bit more headroom on the output avoids clicks when the system is slow to give us our turn
    p.suggestedLatency = std::max(lat, input ? 0.03 : 0.05);
#ifdef _WIN32
    // Converts to 48 kHz mono even if the device works at another rate or in stereo
    PaWasapiStreamInfo wi{};
    const PaHostApiInfo* h = Pa_GetHostApiInfo(info->hostApi);
    if (h && h->type == paWASAPI) {
        wi.size = sizeof wi;
        wi.hostApiType = paWASAPI;
        wi.version = 1;
        wi.flags = paWinWasapiAutoConvert;
        p.hostApiSpecificStreamInfo = &wi;
    }
#endif
    PaStream* s = nullptr;
    if (Pa_OpenStream(&s, input ? &p : nullptr, input ? nullptr : &p, kSampleRate, kFrameSamples, paClipOff, nullptr,
                      nullptr) != paNoError)
        return nullptr;
    if (Pa_StartStream(s) != paNoError) { Pa_CloseStream(s); return nullptr; }
    return s;
}

bool Audio::openStreams() {
    notes.clear();
    if (!ensureInit()) { notes.push_back(T("could not start the audio system")); return false; }
    auto open = [&](bool input, const std::string& choice, std::string& name) -> void* {
        bool fellBack = false;
        int dev = resolve(input, choice, fellBack);
        if (fellBack)
            notes.push_back(std::string(input ? T("the microphone") : T("the output")) + " '" + choice +
                            T("' is not connected; using the default"));
        // The output, in stereo if possible (for the audio of shared screens)
        auto tryOpen = [&](int d) {
            void* st = input ? nullptr : openStream(false, d, 2);
            if (st) { outCh_ = 2; return st; }
            st = openStream(input, d, 1);
            if (st && !input) outCh_ = 1;
            return st;
        };
        void* s = tryOpen(dev);
        if (!s) {
            // Plan B: the default of the system's default API (MME on Windows)
            int alt = input ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();
            if (alt != dev) { s = tryOpen(alt); if (s) dev = alt; }
        }
        if (s) {
            const PaDeviceInfo* d = Pa_GetDeviceInfo(dev);
            name = d ? d->name : "?";
        }
        return s;
    };
    std::string inN, outN;
    in_ = testTone ? nullptr : open(true, inputChoice, inN);
    out_ = open(false, outputChoice, outN);
    {
        std::lock_guard<std::mutex> l(nameMu_);
        inName_ = testTone ? T("(test tone)") : inN;
        outName_ = outN;
    }
    if (!in_ && !testTone) notes.push_back(T("could not open the microphone"));
    if (!out_) notes.push_back(T("could not open the audio output"));
    if (!in_ && !out_ && !testTone) return false;

    running_ = true;
    outOpen_ = out_ != nullptr;
    if (in_ || testTone) capT_ = std::thread([this] { captureLoop(); });
    if (out_) playT_ = std::thread([this] { playbackLoop(); });
    return true;
}

void Audio::closeStreams() {
    running_ = false;
    outOpen_ = false;
    if (capT_.joinable()) capT_.join();
    if (playT_.joinable()) playT_.join();
    for (void* s : {in_, out_})
        if (s) { Pa_StopStream((PaStream*)s); Pa_CloseStream((PaStream*)s); }
    in_ = out_ = nullptr;
    std::lock_guard<std::mutex> l(nameMu_);
    inName_.clear();
    outName_.clear();
}

bool Audio::start(SendFn send) {
    if (started_) return false;
    send_ = std::move(send);
    int err = 0;
    enc_ = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) { enc_ = nullptr; return false; }
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate.load()));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(9));  // best quality; for mono voice it is still ~1 % CPU
    opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(5));
    opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    dn_ = rnnoise_create(nullptr);  // model built into the executable
    started_ = true;
    bool ok = openStreams();
#ifdef _WIN32
    // Even if there is no audio now, if you plug in headphones later they will open on their own
    {
        std::lock_guard<std::mutex> l(watchMu_);
        watchStop_ = false;
    }
    watchT_ = std::thread([this] { watchLoop(); });
#endif
    return ok;
}

bool Audio::restart() {
    if (started_) closeStreams();
    // Restarting PortAudio is the only way for it to see devices connected afterwards
    if (paInit_) { Pa_Terminate(); paInit_ = false; }
    ensureInit();
    if (!started_) return paInit_;
    clearSenders();
    return openStreams();
}

void Audio::stop() {
    if (watchT_.joinable()) {
        {
            std::lock_guard<std::mutex> l(watchMu_);
            watchStop_ = true;
        }
        watchCv_.notify_all();
        watchT_.join();
    }
    if (!started_) return;
    closeStreams();
    if (enc_) opus_encoder_destroy(enc_);
    enc_ = nullptr;
    if (dn_) rnnoise_destroy(dn_);
    dn_ = nullptr;
    started_ = false;
    clearSenders();
}

void Audio::shutdown() {
    stop();
    if (paInit_) { Pa_Terminate(); paInit_ = false; }
}

std::string Audio::describe(bool input) {
    auto v = list(input);
    std::string cur = input ? inputName() : outputName();
    const std::string& choice = input ? inputChoice : outputChoice;
    std::ostringstream os;
    os << (input ? T("Microphones") : T("Outputs (speakers / headphones)")) << ":\n";
    if (v.empty()) os << T("  (none)\n");
    for (size_t i = 0; i < v.size(); ++i) {
        os << (v[i].name == cur ? "  > " : "    ") << (i + 1) << ") " << v[i].name;
        if (v[i].isDefault) os << T("  [Windows default]");
        os << "\n";
    }
    const char* autoText = input ? T("auto (the mic of the headphones in use; otherwise the default, not counting 'Stereo Mix')")
                                 : T("auto (follows the Windows default)");
    os << T("  Chosen: ") << (choice.empty() ? autoText : choice) << "\n";
    return os.str();
}

std::string Audio::listDevicesStandalone() {
    Audio a;
    std::string s = T("Audio system: ") + a.hostApiName() + "\n" + a.describe(true) + a.describe(false);
    a.shutdown();
    return s;
}

void Audio::loadChoice() {
    std::ifstream f(configPath("eco_audio"));
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "entrada") inputChoice = v;
        else if (k == "salida") outputChoice = v;
        else if (k == "ruido") noiseSuppression = v != "0";
        else if (k == "agc") autoGain = v != "0";
        else if (k.rfind("vol.", 0) == 0 && k.size() > 4) nickVolume[k.substr(4)] = float(std::atof(v.c_str()));
    }
}

void Audio::saveChoice() const {
    std::ofstream f(configPath("eco_audio"), std::ios::trunc);
    f << "entrada=" << inputChoice << "\n" << "salida=" << outputChoice << "\n"
      << "ruido=" << (noiseSuppression ? 1 : 0) << "\n"
      << "agc=" << (autoGain ? 1 : 0) << "\n";
    for (auto& [nick, v] : nickVolume) f << "vol." << nick << "=" << v << "\n";
}

#ifdef _WIN32
// Watches Windows' devices: if you plug in or remove headphones, or change the
// default, it signals to reopen the audio. It uses its own COM and doesn't touch PortAudio.
void Audio::watchLoop() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))) || !en) {
        if (SUCCEEDED(co)) CoUninitialize();
        return;
    }
    auto addId = [](IMMDevice* d, std::wstring& s) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id)) && id) { s += id; CoTaskMemFree(id); }
        s += L';';
    };
    auto signature = [&] {
        std::wstring s;
        for (EDataFlow f : {eRender, eCapture}) {
            IMMDevice* d = nullptr;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(f, eConsole, &d)) && d) { addId(d, s); d->Release(); }
            s += L'|';
        }
        IMMDeviceCollection* c = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &c)) && c) {
            UINT n = 0;
            c->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                IMMDevice* d = nullptr;
                if (SUCCEEDED(c->Item(i, &d)) && d) { addId(d, s); d->Release(); }
            }
            c->Release();
        }
        return s;
    };
    std::wstring last = signature();
    bool pending = false;
    std::unique_lock<std::mutex> lk(watchMu_);
    while (!watchStop_) {
        watchCv_.wait_for(lk, std::chrono::milliseconds(1500), [this] { return watchStop_; });
        if (watchStop_) break;
        lk.unlock();
        std::wstring now = signature();
        lk.lock();
        if (now != last) {  // waits for Windows to finish changing things before signalling
            last = now;
            pending = true;
        } else if (pending) {
            pending = false;
            if (onDevicesChanged) onDevicesChanged();
        }
    }
    lk.unlock();
    en->Release();
    if (SUCCEEDED(co)) CoUninitialize();
}
#else
void Audio::watchLoop() {}
#endif

void Audio::captureLoop() {
    raiseThreadPriority();
    std::vector<int16_t> pcm(kFrameSamples);
    std::vector<float> buf(kFrameSamples), clean(kDenoiseFrame);
    // Last silent frames, in case they start talking (fixed-size ring)
    std::vector<std::vector<int16_t>> preroll(kPreroll, std::vector<int16_t>(kFrameSamples));
    int prePos = 0, preN = 0;
    auto rmsOf = [](const float* x) {
        double e = 0;
        for (int i = 0; i < kFrameSamples; ++i) e += double(x[i]) * x[i];
        return float(std::sqrt(e / kFrameSamples) / 32768.0);
    };
    uint8_t pkt[kMaxPacket];
    int hang = 0;
    bool wasTx = false;
    int lastBitrate = bitrate;
    float hpX = 0, hpY = 0;  // high-pass filter state
    float gain = 1.f;        // current gain of the automatic control
    // Applies the gain going smoothly from g0 to g1 (no audible "steps") and without clipping
    auto applyGain = [](int16_t* f, float g0, float g1) {
        if (g0 == 1.f && g1 == 1.f) return;
        for (int i = 0; i < kFrameSamples; ++i) f[i] = softClip(f[i] * (g0 + (g1 - g0) * float(i) / kFrameSamples));
    };
    auto nextTone = std::chrono::steady_clock::now();
    auto encode = [&](const int16_t* frame) {
        int n = opus_encode(enc_, frame, kFrameSamples, pkt, kMaxPacket);
        if (n > 0 && send_) { send_(pkt, size_t(n)); ++txPackets; }
    };
    while (running_) {
        if (testTone) {
            static double ph = 0;
            for (auto& v : pcm) { v = int16_t(8000 * std::sin(ph)); ph += 2 * 3.14159265 * 440 / kSampleRate; }
            nextTone += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(nextTone);
        } else {
            PaError e = Pa_ReadStream((PaStream*)in_, pcm.data(), kFrameSamples);
            if (e != paNoError && e != paInputOverflowed) {
                // e.g. the mic was disconnected: wait for the watcher to reopen it
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
        }
        if (bitrate != lastBitrate) { lastBitrate = bitrate; opus_encoder_ctl(enc_, OPUS_SET_BITRATE(lastBitrate)); }

        bool allowed = !muted && (!pushToTalk || pttHeld);
        if (!allowed) {
            // Mic muted or push-to-talk key not pressed: no filters and no noise suppression, CPU ~0
            micLevel = 0;
            talking = false;
            wasTx = false;
            hang = 0;
            preN = 0;
            continue;
        }

        if (!testTone) {
            // High-pass filter (~80 Hz): removes hum, knocks on the desk and breathing "pops"
            const float a = 0.9896f;
            for (int i = 0; i < kFrameSamples; ++i) {
                float x = pcm[i];
                hpY = a * (hpY + x - hpX);
                hpX = x;
                buf[i] = hpY;
            }
            // Noise suppression: fans, keyboard, traffic... (RNNoise, 10 ms frames).
            // It only removes energy: if before cleaning it already doesn't reach the threshold and we are not talking,
            // it won't reach it afterwards either, so in silence all the work is saved.
            bool quietRaw = !pushToTalk && hang == 0 && rmsOf(buf.data()) < vadThreshold;
            if (noiseSuppression && dn_ && !quietRaw)
                for (int h = 0; h < kFrameSamples; h += kDenoiseFrame) {
                    rnnoise_process_frame(dn_, clean.data(), buf.data() + h);
                    std::copy(clean.begin(), clean.end(), buf.begin() + h);
                }
            for (int i = 0; i < kFrameSamples; ++i) pcm[i] = int16_t(std::clamp(buf[i], -32768.f, 32767.f));
        }

        double sum = 0;
        for (int16_t s : pcm) sum += double(s) * s;
        float rms = float(std::sqrt(sum / kFrameSamples) / 32768.0);
        micLevel = rms;
        bool voice = pushToTalk ? true : rms >= vadThreshold;
        if (voice) hang = kHangover;
        bool tx = voice || hang > 0;
        if (hang > 0 && !voice) --hang;
        talking = tx;
        if (!tx) {  // silence: nothing is sent (saves bandwidth)
            wasTx = false;
            std::copy(pcm.begin(), pcm.end(), preroll[prePos].begin());  // fixed ring: no memory allocation
            prePos = (prePos + 1) % kPreroll;
            preN = std::min(preN + 1, kPreroll);
            continue;
        }
        // Automatic volume: only adjusted with real voice (never raises the noise of silences).
        // Goes down fast if you are too loud and up slowly (~1 s), so it doesn't "pump".
        float g0 = gain;
        if (autoGain) {
            if (rms >= std::max(vadThreshold.load(), 0.005f)) {
                int peak = 1;
                for (int16_t v : pcm) peak = std::max(peak, std::abs(int(v)));
                float want = std::min({kAgcTarget / rms, kAgcMax, 30000.f / float(peak)});
                want = std::max(want, kAgcMin);
                gain += (want - gain) * (want < gain ? 0.25f : 0.02f);
            }
        } else {
            gain = 1.f;
        }
        agcGain = gain;
        // Starts talking: the previous 40 ms are sent first so the first syllable isn't swallowed
        if (!wasTx)
            for (int i = 0; i < preN; ++i) {
                int16_t* f = preroll[(prePos - preN + i + kPreroll) % kPreroll].data();
                applyGain(f, g0, g0);
                encode(f);
            }
        preN = 0;
        wasTx = true;
        applyGain(pcm.data(), g0, gain);
        encode(pcm.data());
    }
}

// Takes the next frame of a person. If their packet is missing: FEC from the next one or, otherwise, Opus concealment.
// Returns false if there is nothing to play (not talking or the cushion is still filling up).
bool Audio::decodeNext(Stream& s, int16_t* out) {
    if (!s.primed) {
        if (s.pkts.empty() || int(s.pkts.size()) < s.target) return false;
        s.primed = true;
        s.next = s.pkts.begin()->first;
        s.plcRun = 0;
    }
    // If too much piled up (e.g. after a network stall), it skips ahead so it doesn't drag delay
    if (int(s.pkts.size()) > s.target + 4) {
        while (int(s.pkts.size()) > s.target + 1) s.pkts.erase(s.pkts.begin());
        s.next = s.pkts.begin()->first;
    }
    int r = 0;
    auto it = s.pkts.find(s.next);
    if (it != s.pkts.end()) {
        r = opus_decode(s.dec, it->second.data(), opus_int32(it->second.size()), out, kFrameSamples, 0);
        s.pkts.erase(it);
        s.plcRun = 0;
        if (s.ch == 1) ++playedFrames;
    } else {
        if (s.pkts.empty() && s.plcRun >= 2) {  // stopped talking: stops after 40 ms of soft tail
            s.primed = false;
            return false;
        }
        if (s.ch == 1 && !s.pkts.empty()) ++lostFrames;  // one is missing mid-sentence: it was lost on the way
        auto nx = s.pkts.find(s.next + 1);
        r = nx != s.pkts.end() ? opus_decode(s.dec, nx->second.data(), opus_int32(nx->second.size()), out, kFrameSamples, 1)
                               : opus_decode(s.dec, nullptr, 0, out, kFrameSamples, 0);
        ++s.plcRun;
    }
    ++s.next;
    // After ~30 s without hiccups the cushion goes down again (less delay)
    if (++s.calm > 1500 && s.target > s.minTarget) { --s.target; s.calm = 0; }
    return r > 0;
}

void Audio::playbackLoop() {
    raiseThreadPriority();
    // Always mixed in stereo (L, R interleaved); voices go the same on both sides
    std::vector<float> mix(kFrameSamples * 2);
    std::vector<int16_t> frame(kFrameSamples * 2), out(kFrameSamples * 2);
    while (running_) {
        std::fill(mix.begin(), mix.end(), 0.f);
        {
            std::lock_guard<std::mutex> l(mu_);
            if (screen_.dec && decodeNext(screen_, frame.data())) {
                float k = screenVolume;
                for (int i = 0; i < kFrameSamples * 2; ++i) mix[i] += frame[i] * k;
            }
            for (auto it = streams_.begin(); it != streams_.end();) {
                Stream& s = it->second;
                if (decodeNext(s, frame.data())) {
                    auto g = senderGain_.find(it->first);
                    float k = g == senderGain_.end() ? 1.f : g->second;
                    for (int i = 0; i < kFrameSamples; ++i) { mix[2 * i] += frame[i] * k; mix[2 * i + 1] += frame[i] * k; }
                    s.idle = 0;
                } else if (++s.idle > 500) {  // quita decodificadores inactivos tras ~10 s
                    opus_decoder_destroy(s.dec);
                    it = streams_.erase(it);
                    continue;
                }
                ++it;
            }
        }
        float vol = deafened ? 0.f : volume.load();
        if (outCh_ == 2)
            for (int i = 0; i < kFrameSamples * 2; ++i) out[i] = softClip(mix[i] * vol);
        else
            for (int i = 0; i < kFrameSamples; ++i) out[i] = softClip((mix[2 * i] + mix[2 * i + 1]) * 0.5f * vol);
        PaError e = Pa_WriteStream((PaStream*)out_, out.data(), kFrameSamples);
        if (e != paNoError && e != paOutputUnderflowed) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void Audio::onPacket(uint32_t sender, uint16_t seq, const uint8_t* data, size_t n) {
    ++rxPackets;
    if (!outOpen_ || n == 0) return;
    std::lock_guard<std::mutex> l(mu_);
    Stream& s = streams_[sender];
    if (!s.dec) {
        int err = 0;
        s.dec = opus_decoder_create(kSampleRate, 1, &err);
        if (err != OPUS_OK) { streams_.erase(sender); return; }
        s.minTarget = s.target = kJitterStart;
        s.maxTarget = kJitterTargetMax;
        s.maxPkts = kJitterMax;
    }
    push(s, seq, data, n);
}

void Audio::push(Stream& s, uint16_t seq, const uint8_t* data, size_t n) {
    // Sequence number without the 16-bit wraparound
    int64_t ext = s.started ? s.last + int16_t(uint16_t(seq - uint16_t(s.last))) : int64_t(seq) + 65536;
    if (!s.started || ext > s.last) s.last = ext;
    s.started = true;
    if (s.primed && ext < s.next) {
        // It arrived when it already had to be made up: the network is choppy, more cushion
        if (s.target < s.maxTarget) ++s.target;
        s.calm = 0;
        return;
    }
    s.pkts.emplace(ext, std::vector<uint8_t>(data, data + n));
    while (int(s.pkts.size()) > s.maxPkts) s.pkts.erase(s.pkts.begin());
}

void Audio::onScreenAudio(uint32_t sender, uint16_t seq, const uint8_t* data, size_t n) {
    if (!outOpen_ || n == 0 || n > size_t(kMaxPacket)) return;  // an Opus packet is never larger than 1275 bytes
    std::lock_guard<std::mutex> l(mu_);
    if (sender != screenFrom_ || !screen_.dec) {
        if (screen_.dec) opus_decoder_destroy(screen_.dec);
        screen_ = Stream{};
        int err = 0;
        screen_.dec = opus_decoder_create(kSampleRate, 2, &err);
        if (err != OPUS_OK) { screen_.dec = nullptr; return; }
        // Arrives 3 frames at a time (every 60 ms): the cushion has to cover at least one packet and some jitter
        screen_.ch = 2;
        screen_.minTarget = screen_.target = 5;  // 100 ms
        screen_.maxTarget = 12;
        screen_.maxPkts = 30;
        screenFrom_ = sender;
    }
    push(screen_, seq, data, n);
}

void Audio::clearScreenAudio() {
    std::lock_guard<std::mutex> l(mu_);
    if (screen_.dec) opus_decoder_destroy(screen_.dec);
    screen_ = Stream{};
    screenFrom_ = 0;
}

void Audio::setSenderGain(uint32_t sender, float gain) {
    std::lock_guard<std::mutex> l(mu_);
    if (std::fabs(gain - 1.f) < 0.01f) senderGain_.erase(sender);
    else senderGain_[sender] = std::clamp(gain, 0.f, 3.f);
}

void Audio::removeSender(uint32_t sender) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = streams_.find(sender);
    if (it == streams_.end()) return;
    opus_decoder_destroy(it->second.dec);
    streams_.erase(it);
}

void Audio::clearSenders() {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& [id, s] : streams_) opus_decoder_destroy(s.dec);
    streams_.clear();
    if (screen_.dec) opus_decoder_destroy(screen_.dec);
    screen_ = Stream{};
    screenFrom_ = 0;
}

}  // namespace eco
