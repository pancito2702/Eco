#include "loopback.hpp"
#include "../common/i18n.hpp"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <audiopolicy.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

#include "../common/protocol.hpp"

namespace eco {

namespace {

// audioclientactivationparams.h (not in every MinGW)
struct ProcessLoopbackParams {
    DWORD activationType;  // 1 = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
    DWORD targetProcessId;
    DWORD loopbackMode;    // 1 = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
};
const GUID kIidAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
const GUID kIidAudioClient = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
const GUID kIidAudioCaptureClient = {0xc8adbd64, 0xe71e, 0x48a0, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
const GUID kIidActivateHandler = {0x41d949ab, 0x9862, 0x444a, {0x80, 0xf6, 0xc2, 0x61, 0x33, 0x4d, 0xa5, 0xeb}};

using ActivateFn = HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
                                    IActivateAudioInterfaceAsyncOperation**);

// Receives the result of ActivateAudioInterfaceAsync (arrives on another thread; that's why it is "agile")
class ActivateHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    ActivateHandler() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ActivateHandler() { if (done) CloseHandle(done); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, kIidActivateHandler) || IsEqualGUID(riid, kIidAgileObject)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return ULONG(n);
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT r = E_FAIL;
        IUnknown* u = nullptr;
        if (SUCCEEDED(op->GetActivateResult(&r, &u)) && SUCCEEDED(r) && u) {
            u->QueryInterface(kIidAudioClient, reinterpret_cast<void**>(&client));
        }
        if (u) u->Release();
        SetEvent(done);
        return S_OK;
    }
    HANDLE done = nullptr;
    IAudioClient* client = nullptr;

private:
    LONG ref_ = 1;
};

WAVEFORMATEX wantedFormat() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = kSampleRate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = kSampleRate * 4;
    return f;
}

constexpr REFERENCE_TIME kBuffer = 2000000;  // 200 ms

// include = false: everything playing except that process (Eco). include = true: only that process and its children
IAudioClient* openProcessLoopback(HANDLE event, DWORD pid, bool include) {
    HMODULE m = LoadLibraryExW(L"mmdevapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);  // never from the program folder
    auto activate = m ? reinterpret_cast<ActivateFn>(GetProcAddress(m, "ActivateAudioInterfaceAsync")) : nullptr;
    if (!activate) return nullptr;
    ProcessLoopbackParams p{1, pid, include ? 0u : 1u};
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof p;
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&p);
    auto* h = new ActivateHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    IAudioClient* c = nullptr;
    if (h->done && SUCCEEDED(activate(L"VAD\\Process_Loopback", kIidAudioClient, &pv, h, &op)) &&
        WaitForSingleObject(h->done, 3000) == WAIT_OBJECT_0) {
        c = h->client;
        h->client = nullptr;
    }
    if (op) op->Release();
    h->Release();
    if (!c) return nullptr;
    WAVEFORMATEX f = wantedFormat();
    if (FAILED(c->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBuffer, 0,
                             &f, nullptr)) ||
        FAILED(c->SetEventHandle(event))) {
        c->Release();
        return nullptr;
    }
    return c;
}

// Plan B (old Windows versions): everything going out through the default output, Eco included
IAudioClient* openDeviceLoopback() {
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))) || !en)
        return nullptr;
    IMMDevice* d = nullptr;
    IAudioClient* c = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d)) && d) {
        d->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&c));
        d->Release();
    }
    en->Release();
    if (!c) return nullptr;
    WAVEFORMATEX f = wantedFormat();
    if (FAILED(c->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                             kBuffer, 0, &f, nullptr))) {
        c->Release();
        return nullptr;
    }
    return c;
}

std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 1 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string lowerAscii(std::string s) {
    for (auto& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// All processes: pid -> (parent, lowercase .exe name)
std::map<DWORD, std::pair<DWORD, std::string>> processes() {
    std::map<DWORD, std::pair<DWORD, std::string>> m;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return m;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        m[pe.th32ProcessID] = {pe.th32ParentProcessID, lowerAscii(narrow(pe.szExeFile))};
    CloseHandle(snap);
    return m;
}

// Processes with a sound session open on the default output
std::set<DWORD> audioPids() {
    std::set<DWORD> pids;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))) || !en)
        return pids;
    IMMDevice* d = nullptr;
    IAudioSessionManager2* mgr = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d)) && d) {
        d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr));
        d->Release();
    }
    en->Release();
    if (!mgr) return pids;
    IAudioSessionEnumerator* se = nullptr;
    if (SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se) {
        int n = 0;
        se->GetCount(&n);
        for (int i = 0; i < n; ++i) {
            IAudioSessionControl* ctl = nullptr;
            if (FAILED(se->GetSession(i, &ctl)) || !ctl) continue;
            IAudioSessionControl2* c2 = nullptr;
            if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&c2))) && c2) {
                DWORD pid = 0;
                if (SUCCEEDED(c2->GetProcessId(&pid)) && pid && pid != GetCurrentProcessId()) pids.insert(pid);
                c2->Release();
            }
            ctl->Release();
        }
        se->Release();
    }
    mgr->Release();
    return pids;
}

// The process to capture for "chrome.exe": the highest one of its family with that same name (so all
// its tabs and audio processes are included), starting from one that has sound open
DWORD rootPidOf(const std::string& app) {
    std::string want = lowerAscii(app);
    auto procs = processes();
    for (DWORD pid : audioPids()) {
        auto it = procs.find(pid);
        if (it == procs.end() || it->second.second != want) continue;
        DWORD root = pid;
        for (int guard = 0; guard < 64; ++guard) {
            auto p = procs.find(procs[root].first);
            if (p == procs.end() || p->first == root || p->second.second != want) break;
            root = p->first;
        }
        return root;
    }
    return 0;
}

class WasapiLoopback : public SystemAudioCapture {
public:
    ~WasapiLoopback() override {
        if (client_) client_->Stop();
        if (cap_) cap_->Release();
        if (client_) client_->Release();
        if (event_) CloseHandle(event_);
        if (comOk_) CoUninitialize();
    }

    bool open(std::string& note, const std::string& app) {
        comOk_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!app.empty()) {  // only one application: needs per-process capture (Windows 10 2004 or later)
            DWORD pid = rootPidOf(app);
            if (!pid) { note = "«" + app + T("» does not have sound open right now"); return false; }
            if (event_) client_ = openProcessLoopback(event_, pid, true);
            if (!client_) { note = T("this Windows doesn't allow sharing the sound of a single application"); return false; }
            self_ = true;
            return start(note);
        }
        if (event_) client_ = openProcessLoopback(event_, GetCurrentProcessId(), false);
        if (client_) {
            self_ = true;
        } else {
            CloseHandle(event_);
            event_ = nullptr;
            client_ = openDeviceLoopback();
        }
        if (!client_) { note = T("could not capture the PC audio"); return false; }
        return start(note);
    }

    bool start(std::string& note) {
        if (FAILED(client_->GetService(kIidAudioCaptureClient, reinterpret_cast<void**>(&cap_))) || !cap_ ||
            FAILED(client_->Start())) {
            note = T("could not start capturing the PC audio");
            return false;
        }
        return true;
    }

    bool read(std::vector<int16_t>& out) override {
        if (event_) WaitForSingleObject(event_, 20);
        else Sleep(10);
        for (;;) {
            UINT32 n = 0;
            HRESULT hr = cap_->GetNextPacketSize(&n);
            if (FAILED(hr)) return false;  // e.g. the output was changed or disconnected
            if (n == 0) return true;
            BYTE* data = nullptr;
            DWORD flags = 0;
            if (FAILED(cap_->GetBuffer(&data, &n, &flags, nullptr, nullptr))) return false;
            size_t at = out.size();
            out.resize(at + size_t(n) * 2);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) std::fill(out.begin() + at, out.end(), int16_t(0));
            else memcpy(out.data() + at, data, size_t(n) * 4);
            cap_->ReleaseBuffer(n);
        }
    }

    bool excludesSelf() const override { return self_; }

private:
    bool comOk_ = false, self_ = false;
    HANDLE event_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* cap_ = nullptr;
};

}  // namespace

std::unique_ptr<SystemAudioCapture> SystemAudioCapture::create(std::string& note, const std::string& app) {
    auto c = std::make_unique<WasapiLoopback>();
    if (!c->open(note, app)) return nullptr;
    return c;
}

std::vector<AudioApp> SystemAudioCapture::apps() {
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    std::vector<AudioApp> v;
    auto procs = processes();
    std::set<std::string> seen;
    for (DWORD pid : audioPids()) {
        auto it = procs.find(pid);
        if (it == procs.end() || it->second.second.empty() || !seen.insert(it->second.second).second) continue;
        v.push_back({it->second.second, it->second.second});
    }
    if (com) CoUninitialize();
    return v;
}

}  // namespace eco

#elif defined(__linux__)

// Linux: the "monitor" of PulseAudio's default output (also with PipeWire, which mimics it). libpulse-simple
// is loaded at runtime: if it isn't installed, Eco works the same and only warns when trying to share audio.
#include <dlfcn.h>

#include "../common/protocol.hpp"

namespace eco {

namespace {

struct PaSampleSpec { int format; uint32_t rate; uint8_t channels; };          // pa_sample_spec
struct PaBufferAttr { uint32_t maxlength, tlength, prebuf, minreq, fragsize; };  // pa_buffer_attr
constexpr int kPaSampleS16LE = 3, kPaStreamRecord = 2;
using NewFn = void* (*)(const char*, const char*, int, const char*, const char*, const PaSampleSpec*, const void*,
                        const PaBufferAttr*, int*);
using ReadFn = int (*)(void*, void*, size_t, int*);
using FreeFn = void (*)(void*);

class PulseMonitor : public SystemAudioCapture {
public:
    ~PulseMonitor() override {
        if (s_) free_(s_);
        if (lib_) dlclose(lib_);
    }
    bool open(std::string& note) {
        lib_ = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!lib_) { note = T("libpulse-simple is missing (install PulseAudio or pipewire-pulse)"); return false; }
        auto nw = reinterpret_cast<NewFn>(dlsym(lib_, "pa_simple_new"));
        read_ = reinterpret_cast<ReadFn>(dlsym(lib_, "pa_simple_read"));
        free_ = reinterpret_cast<FreeFn>(dlsym(lib_, "pa_simple_free"));
        if (!nw || !read_ || !free_) { note = T("libpulse-simple is not the expected one"); return false; }
        PaSampleSpec ss{kPaSampleS16LE, uint32_t(kSampleRate), 2};
        uint32_t frag = uint32_t(kFrameSamples) * 4;  // 20 ms of 16-bit stereo
        PaBufferAttr ba{uint32_t(-1), uint32_t(-1), uint32_t(-1), uint32_t(-1), frag};
        int err = 0;
        s_ = nw(nullptr, T("Eco"), kPaStreamRecord, "@DEFAULT_MONITOR@", T("Shared screen audio"), &ss, nullptr, &ba, &err);
        if (!s_) { note = T("could not open the monitor of the audio output (PulseAudio/PipeWire)"); return false; }
        return true;
    }
    bool read(std::vector<int16_t>& out) override {
        size_t at = out.size();
        out.resize(at + size_t(kFrameSamples) * 2);
        int err = 0;
        if (read_(s_, out.data() + at, size_t(kFrameSamples) * 4, &err) < 0) { out.resize(at); return false; }
        return true;
    }
    bool excludesSelf() const override { return false; }  // the monitor carries everything, including what Eco plays

private:
    void* lib_ = nullptr;
    void* s_ = nullptr;
    ReadFn read_ = nullptr;
    FreeFn free_ = nullptr;
};

}  // namespace

std::vector<AudioApp> SystemAudioCapture::apps() { return {}; }
std::unique_ptr<SystemAudioCapture> SystemAudioCapture::create(std::string& note, const std::string& app) {
    if (!app.empty()) { note = T("on Linux, for now only all the sound can be shared, not a single application"); return nullptr; }
    auto c = std::make_unique<PulseMonitor>();
    if (!c->open(note)) return nullptr;
    return c;
}

}  // namespace eco

#else

namespace eco {
std::vector<AudioApp> SystemAudioCapture::apps() { return {}; }
std::unique_ptr<SystemAudioCapture> SystemAudioCapture::create(std::string& note, const std::string&) {
    note = T("sharing the PC audio doesn't work on this system yet (it does on Windows and Linux)");
    return nullptr;
}
}  // namespace eco

#endif
