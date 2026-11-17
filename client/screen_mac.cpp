// macOS: global keys for push-to-talk (CoreGraphics).
// Pending: capturing the screen (Apple requires ScreenCaptureKit since macOS 14). Watching other people's
// screens does work in eco-gui, which draws it inside its own window.
#include <ApplicationServices/ApplicationServices.h>
#include "../common/i18n.hpp"

#include <cctype>
#include <cstdlib>
#include <string>

#include "screen.hpp"

namespace eco {

namespace {

class NoCapturer : public ScreenCapturer {
public:
    bool capture(Image&) override { return false; }
};

// The macOS console has no viewer of its own: use eco-gui to watch screens
class NoViewer : public Viewer {
public:
    void open(const std::string&) override {}
    void close() override {}
    bool isOpen() const override { return false; }
    void present(const Image&) override {}
};

int keyCode(const std::string& k) {
    // macOS ANSI virtual key codes
    static const char* letters = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.";
    if (k.size() == 1) {
        char c = char(std::tolower((unsigned char)k[0]));
        for (int i = 0; i < 48; ++i)
            if (letters[i] == c) return i;
    }
    if (k == "espacio" || k == "space") return 49;
    if (k == "ctrl") return 59;
    if (k == "alt") return 58;
    if (k == "shift") return 56;
    if (k == "capslock" || k == "mayus") return 57;
    static const int fkeys[] = {122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111};
    if (k.size() >= 2 && (k[0] == 'f' || k[0] == 'F') && std::isdigit((unsigned char)k[1])) {
        int n = std::atoi(k.c_str() + 1);
        if (n >= 1 && n <= 12) return fkeys[n - 1];
    }
    return -1;
}

}  // namespace

bool globalKeyDown(const std::string& k) {
    if (k == "mouse4") return CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, CGMouseButton(3));
    if (k == "mouse5") return CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, CGMouseButton(4));
    int code = keyCode(k);
    if (code < 0) return false;
    // Requires the «Input Monitoring» permission in System Settings > Privacy
    return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, CGKeyCode(code));
}

std::vector<CaptureSource> ScreenCapturer::sources() { return {{"pantalla:0", T("Full screen")}}; }
std::unique_ptr<ScreenCapturer> ScreenCapturer::create(const std::string&) { return std::make_unique<NoCapturer>(); }
std::unique_ptr<Viewer> Viewer::create() { return std::make_unique<NoViewer>(); }

}  // namespace eco
