// Capture of "what plays on the PC" to send it along with the shared screen.
// Windows: WASAPI. First per-process capture is tried (Windows 10 2004 or later) excluding Eco,
// so viewers don't hear themselves; if that's not possible, everything playing on the default output.
// On other systems it isn't available yet (create returns nullptr).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eco {

// An application that has sound open right now (Windows)
struct AudioApp {
    std::string exe;    // "chrome.exe": what is remembered and passed to create()
    std::string label;  // for display
};

class SystemAudioCapture {
public:
    // Applications with sound open (to choose which one to share). Empty where choosing isn't possible.
    static std::vector<AudioApp> apps();
    // nullptr if not possible; the reason is left in note. Call from the thread that will later do read().
    // app: "" = everything playing except Eco; "chrome.exe" = only that application (and its child processes)
    static std::unique_ptr<SystemAudioCapture> create(std::string& note, const std::string& app = "");
    virtual ~SystemAudioCapture() = default;
    // Waits at most ~20 ms and appends what was captured to out: 48 kHz, interleaved stereo, 16 bits.
    // While nothing plays it may append nothing. false = the device stopped working (it has to be recreated).
    virtual bool read(std::vector<int16_t>& out) = 0;
    // true if Eco's own sound (the channel's voices) is left out
    virtual bool excludesSelf() const = 0;
};

}  // namespace eco
