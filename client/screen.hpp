// Minimal platform layer: screen capture and viewer window.
// Implemented on Win32/GDI (screen_win.cpp) and X11 (screen_x11.cpp).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace eco {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;  // 4 bytes per pixel, rows from top to bottom
};

// What can be shared: id "pantalla:N" (0 = the main one) or "ventana:..." (Windows). "" = main screen
struct CaptureSource {
    std::string id, label;
};

class ScreenCapturer {
public:
    static std::vector<CaptureSource> sources();
    static std::unique_ptr<ScreenCapturer> create(const std::string& source = "");
    virtual ~ScreenCapturer() = default;
    virtual bool capture(Image& out) = 0;
    // true if the last capture is identical to the previous one (DXGI knows without comparing): no need to encode it
    virtual bool unchanged() const { return false; }
    virtual std::string backend() const { return ""; }  // how it is captured, for messages ("DXGI", "GDI"...)
};

// Window that shows an Image. Runs its own event thread.
class Viewer {
public:
    static std::unique_ptr<Viewer> create();
    virtual ~Viewer() = default;
    virtual void open(const std::string& title) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    // Copies the canvas and asks for a repaint (safe from any thread)
    virtual void present(const Image& img) = 0;
};

// Is the key pressed right now? (global, without focus). Names: "v", "f8",
// "ctrl", "alt", "shift", "mouse4", "mouse5"... For push-to-talk.
bool globalKeyDown(const std::string& key);

}  // namespace eco
