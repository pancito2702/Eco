// Capture and viewer for Linux (X11). No dependencies besides libX11.
#include <X11/Xlib.h>
#include "../common/i18n.hpp"
#include <X11/Xutil.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>

#include "screen.hpp"

namespace eco {

namespace {

class X11Capturer : public ScreenCapturer {
public:
    X11Capturer() { dpy_ = XOpenDisplay(nullptr); }
    ~X11Capturer() override { if (dpy_) XCloseDisplay(dpy_); }

    bool capture(Image& out) override {
        if (!dpy_) return false;
        Window root = DefaultRootWindow(dpy_);
        XWindowAttributes a;
        XGetWindowAttributes(dpy_, root, &a);
        XImage* img = XGetImage(dpy_, root, 0, 0, a.width, a.height, AllPlanes, ZPixmap);
        if (!img) return false;
        out.w = a.width;
        out.h = a.height;
        out.bgra.resize(size_t(out.w) * out.h * 4);
        if (img->bits_per_pixel == 32) {
            for (int y = 0; y < out.h; ++y)
                std::memcpy(&out.bgra[size_t(y) * out.w * 4], img->data + size_t(y) * img->bytes_per_line, size_t(out.w) * 4);
        } else {
            for (int y = 0; y < out.h; ++y)
                for (int x = 0; x < out.w; ++x) {
                    unsigned long p = XGetPixel(img, x, y);
                    uint8_t* d = &out.bgra[(size_t(y) * out.w + x) * 4];
                    d[0] = uint8_t(p & img->blue_mask);
                    d[1] = uint8_t((p & img->green_mask) >> 8);
                    d[2] = uint8_t((p & img->red_mask) >> 16);
                    d[3] = 255;
                }
        }
        XDestroyImage(img);
        return true;
    }

private:
    Display* dpy_ = nullptr;
};

class X11Viewer : public Viewer {
public:
    ~X11Viewer() override { close(); }

    void open(const std::string& title) override {
        close();
        running_ = true;
        title_ = title;
        thread_ = std::thread([this] { loop(); });
    }

    void close() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    bool isOpen() const override { return running_; }

    void present(const Image& img) override {
        std::lock_guard<std::mutex> l(mu_);
        frame_ = img;
        dirty_ = true;
    }

private:
    void loop() {
        Display* dpy = XOpenDisplay(nullptr);
        if (!dpy) { running_ = false; return; }
        int scr = DefaultScreen(dpy);
        Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, 1024, 576, 0, 0, BlackPixel(dpy, scr));
        XStoreName(dpy, win, title_.c_str());
        Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dpy, win, &wmDelete, 1);
        XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
        XMapWindow(dpy, win);
        GC gc = XCreateGC(dpy, win, 0, nullptr);
        int ww = 1024, wh = 576;
        std::vector<uint8_t> scaled;
        bool needPaint = false;

        while (running_) {
            while (XPending(dpy)) {
                XEvent e;
                XNextEvent(dpy, &e);
                if (e.type == ClientMessage && Atom(e.xclient.data.l[0]) == wmDelete) running_ = false;
                if (e.type == ConfigureNotify) { ww = e.xconfigure.width; wh = e.xconfigure.height; needPaint = true; }
                if (e.type == Expose) needPaint = true;
            }
            {
                std::lock_guard<std::mutex> l(mu_);
                if (dirty_) { dirty_ = false; needPaint = true; local_ = frame_; }
            }
            if (needPaint && local_.w > 0 && ww > 0 && wh > 0) {
                needPaint = false;
                // Scales keeping the aspect ratio (nearest neighbour, cheap)
                double s = std::min(double(ww) / local_.w, double(wh) / local_.h);
                int dw = std::max(1, int(local_.w * s)), dh = std::max(1, int(local_.h * s));
                scaled.assign(size_t(dw) * dh * 4, 0);
                for (int y = 0; y < dh; ++y) {
                    int sy = std::min(local_.h - 1, int(y / s));
                    const uint8_t* srow = &local_.bgra[size_t(sy) * local_.w * 4];
                    uint8_t* drow = &scaled[size_t(y) * dw * 4];
                    for (int x = 0; x < dw; ++x) {
                        int sx = std::min(local_.w - 1, int(x / s));
                        std::memcpy(drow + x * 4, srow + sx * 4, 4);
                    }
                }
                XImage* xi = XCreateImage(dpy, DefaultVisual(dpy, scr), DefaultDepth(dpy, scr), ZPixmap, 0,
                                          (char*)scaled.data(), dw, dh, 32, dw * 4);
                if (xi) {
                    XClearWindow(dpy, win);
                    XPutImage(dpy, win, gc, xi, 0, 0, (ww - dw) / 2, (wh - dh) / 2, dw, dh);
                    xi->data = nullptr;  // the buffer is ours
                    XDestroyImage(xi);
                }
                XFlush(dpy);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        XFreeGC(dpy, gc);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
    }

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::string title_;
    std::mutex mu_;
    Image frame_, local_;
    bool dirty_ = false;
};

}  // namespace

bool globalKeyDown(const std::string& k) {
    // Only called from the push-to-talk thread
    static Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return false;
    if (k == "mouse4" || k == "mouse5") return false;  // not supported on X11 without extensions
    std::string name = k == "ctrl" ? "Control_L" : k == "alt" ? "Alt_L" : k == "shift" ? "Shift_L"
                     : k == "espacio" || k == "space" ? "space" : k == "capslock" || k == "mayus" ? "Caps_Lock" : k;
    if (name.size() >= 2 && name[0] == 'f' && std::isdigit((unsigned char)name[1])) name[0] = 'F';
    KeySym ks = XStringToKeysym(name.c_str());
    if (ks == NoSymbol) return false;
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    char keys[32];
    XQueryKeymap(dpy, keys);
    return kc && (keys[kc / 8] & (1 << (kc % 8)));
}

std::vector<CaptureSource> ScreenCapturer::sources() { return {{"pantalla:0", T("Full screen")}}; }
std::unique_ptr<ScreenCapturer> ScreenCapturer::create(const std::string&) { return std::make_unique<X11Capturer>(); }
std::unique_ptr<Viewer> Viewer::create() { return std::make_unique<X11Viewer>(); }

}  // namespace eco
