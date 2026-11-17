// Capture and viewer for Windows (Win32 + GDI). No external dependencies.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../common/i18n.hpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <condition_variable>
#include <cstring>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "screen.hpp"

namespace eco {

namespace {

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// A 32-bit DIB where the image is composed; the cursor is drawn there with GDI
class Dib {
public:
    ~Dib() { release(); }
    bool ensure(int w, int h) {
        if (w == w_ && h == h_ && mem_) return true;
        release();
        HDC screen = GetDC(nullptr);
        mem_ = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top to bottom
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!mem_ || !bmp_) { release(); return false; }
        old_ = SelectObject(mem_, bmp_);
        w_ = w;
        h_ = h;
        return true;
    }
    void release() {
        if (mem_) { if (old_) SelectObject(mem_, old_); DeleteDC(mem_); mem_ = nullptr; }
        if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }
        old_ = nullptr;
        bits_ = nullptr;
        w_ = h_ = 0;
    }
    // Draws the mouse cursor if it falls inside (ox, oy = corner of what was captured in screen coordinates)
    void drawCursor(int ox, int oy) {
        CURSORINFO ci{sizeof(ci)};
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return;
        ICONINFO ii;
        if (!GetIconInfo(ci.hCursor, &ii)) return;
        DrawIconEx(mem_, ci.ptScreenPos.x - ox - int(ii.xHotspot), ci.ptScreenPos.y - oy - int(ii.yHotspot), ci.hCursor, 0, 0,
                   0, nullptr, DI_NORMAL);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        GdiFlush();
    }
    void copyTo(Image& out) const {
        out.w = w_;
        out.h = h_;
        out.bgra.resize(size_t(w_) * h_ * 4);
        std::memcpy(out.bgra.data(), bits_, out.bgra.size());
    }
    HDC dc() const { return mem_; }
    uint8_t* bits() const { return static_cast<uint8_t*>(bits_); }
    int w() const { return w_; }
    int h() const { return h_; }

private:
    int w_ = 0, h_ = 0;
    HDC mem_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
};

POINT cursorPos() {
    POINT p{-100000, -100000};
    CURSORINFO ci{sizeof(ci)};
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) p = ci.ptScreenPos;
    return p;
}

// Rectangle of a monitor (in real pixels: the process is DPI-aware)
struct MonitorInfo { HMONITOR h; RECT r; bool primary; };
std::vector<MonitorInfo> monitors() {
    std::vector<MonitorInfo> v;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(m, &mi))
            reinterpret_cast<std::vector<MonitorInfo>*>(p)->push_back({m, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&v));
    // The main one first; the others from left to right
    std::stable_sort(v.begin(), v.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.primary != b.primary) return a.primary;
        return a.r.left < b.r.left;
    });
    return v;
}

// GDI: what works on any Windows (also over Remote Desktop), but uses more CPU
class GdiCapturer : public ScreenCapturer {
public:
    explicit GdiCapturer(RECT r) : r_(r) {}
    bool capture(Image& out) override {
        int w = r_.right - r_.left, h = r_.bottom - r_.top;
        if (w <= 0 || h <= 0 || !dib_.ensure(w, h)) return false;
        HDC screen = GetDC(nullptr);
        BOOL ok = BitBlt(dib_.dc(), 0, 0, w, h, screen, r_.left, r_.top, SRCCOPY | CAPTUREBLT);
        ReleaseDC(nullptr, screen);
        if (!ok) return false;
        GdiFlush();
        dib_.drawCursor(r_.left, r_.top);
        dib_.copyTo(out);
        return true;
    }
    std::string backend() const override { return "GDI"; }

private:
    RECT r_;
    Dib dib_;
};

// A specific window (even if covered by others). PW_RENDERFULLCONTENT also captures those that
// draw with DirectX (browsers, windowed games) on Windows 8.1 or later
class WindowCapturer : public ScreenCapturer {
public:
    explicit WindowCapturer(HWND w) : hwnd_(w) {}
    bool capture(Image& out) override {
        if (!IsWindow(hwnd_) || IsIconic(hwnd_)) return false;
        RECT r;
        if (!GetWindowRect(hwnd_, &r)) return false;
        int w = r.right - r.left, h = r.bottom - r.top;
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || !dib_.ensure(w, h)) return false;
        if (!PrintWindow(hwnd_, dib_.dc(), 0x2 /* PW_RENDERFULLCONTENT */)) return false;
        GdiFlush();
        // PrintWindow leaves the alpha channel at 0 in many windows: the codec doesn't use it, but this way there are no surprises
        uint8_t* p = dib_.bits();
        for (size_t i = 3, n = size_t(w) * h * 4; i < n; i += 4) p[i] = 255;
        dib_.drawCursor(r.left, r.top);
        dib_.copyTo(out);
        return true;
    }
    std::string backend() const override { return "ventana"; }

private:
    HWND hwnd_;
    Dib dib_;
};

// DXGI Desktop Duplication: Windows gives Eco only the frames that change, already on the GPU. It uses much
// less CPU than GDI, especially in 4K or at many fps. If it isn't possible (Remote Desktop, some laptops with
// dos GPU, Windows antiguos...), create() usa GDI.
class DxgiCapturer : public ScreenCapturer {
public:
    ~DxgiCapturer() override { releaseAll(); }

    bool init(HMONITOR mon, RECT r) {
        mon_ = mon;
        r_ = r;
        return reopen();
    }

    bool capture(Image& out) override {
        unchanged_ = false;
        if (!dup_ && !reopen()) return false;
        bool newFrame = false;
        // Normally there's no waiting (the caller sets the pace). The first time, yes, a little: a base image
        // is needed, and DXGI sometimes starts with "frames" that only carry the mouse (LastPresentTime = 0)
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(haveBase_ ? 0 : 400);
        for (;;) {
            DXGI_OUTDUPL_FRAME_INFO fi{};
            IDXGIResource* res = nullptr;
            HRESULT hr = dup_->AcquireNextFrame(haveBase_ ? 0 : 100, &fi, &res);
            if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
                // Resolution change, UAC screen, computer locked...: it is reopened on the next one
                releaseDup();
                return haveBase_ ? compose(out, false) : false;
            }
            if (FAILED(hr) && hr != DXGI_ERROR_WAIT_TIMEOUT) { releaseDup(); return false; }
            if (SUCCEEDED(hr)) {
                if (fi.LastPresentTime.QuadPart != 0) newFrame = copyFrame(res) || newFrame;
                res->Release();
                dup_->ReleaseFrame();
            }
            if (haveBase_ || std::chrono::steady_clock::now() >= deadline) break;
        }
        // With a completely still screen none may arrive: the base image is taken with GDI once
        if (!haveBase_ && !gdiBase()) return false;
        return compose(out, newFrame);
    }
    bool unchanged() const override { return unchanged_; }
    std::string backend() const override { return "DXGI"; }

private:
    bool reopen() {
        releaseAll();
        IDXGIFactory1* f = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) || !f) return false;
        IDXGIAdapter1* ad = nullptr;
        IDXGIOutput* found = nullptr;
        IDXGIAdapter1* foundAd = nullptr;
        for (UINT i = 0; !found && f->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
            IDXGIOutput* o = nullptr;
            for (UINT j = 0; !found && ad->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j) {
                DXGI_OUTPUT_DESC d;
                if (SUCCEEDED(o->GetDesc(&d)) && d.Monitor == mon_) {
                    found = o;
                    foundAd = ad;
                    ad->AddRef();
                } else {
                    o->Release();
                }
            }
            ad->Release();
        }
        f->Release();
        if (!found) return false;
        IDXGIOutput1* o1 = nullptr;
        HRESULT hr = found->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&o1));
        found->Release();
        if (FAILED(hr) || !o1) { foundAd->Release(); return false; }
        hr = D3D11CreateDevice(foundAd, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
        foundAd->Release();
        if (SUCCEEDED(hr)) hr = o1->DuplicateOutput(dev_, &dup_);
        o1->Release();
        if (FAILED(hr) || !dup_) { releaseAll(); return false; }
        return true;
    }

    bool gdiBase() {
        int w = r_.right - r_.left, h = r_.bottom - r_.top;
        if (w <= 0 || h <= 0 || !dib_.ensure(w, h)) return false;
        HDC screen = GetDC(nullptr);
        BOOL ok = BitBlt(dib_.dc(), 0, 0, w, h, screen, r_.left, r_.top, SRCCOPY | CAPTUREBLT);
        ReleaseDC(nullptr, screen);
        if (!ok) return false;
        GdiFlush();
        base_.assign(dib_.bits(), dib_.bits() + size_t(w) * h * 4);
        sw_ = w;
        sh_ = h;
        haveBase_ = true;
        return true;
    }

    // From the GPU texture to base_ (without cursor)
    bool copyFrame(IDXGIResource* res) {
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex) return false;
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        if (!staging_ || int(d.Width) != sw_ || int(d.Height) != sh_) {
            if (staging_) { staging_->Release(); staging_ = nullptr; }
            D3D11_TEXTURE2D_DESC s = d;
            s.Usage = D3D11_USAGE_STAGING;
            s.BindFlags = 0;
            s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            s.MiscFlags = 0;
            s.MipLevels = 1;
            s.ArraySize = 1;
            s.SampleDesc.Count = 1;
            s.SampleDesc.Quality = 0;
            if (FAILED(dev_->CreateTexture2D(&s, nullptr, &staging_))) { staging_ = nullptr; tex->Release(); return false; }
            sw_ = int(d.Width);
            sh_ = int(d.Height);
        }
        ctx_->CopyResource(staging_, tex);
        tex->Release();
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx_->Map(staging_, 0, D3D11_MAP_READ, 0, &m))) return false;
        base_.resize(size_t(sw_) * sh_ * 4);
        const uint8_t* src = static_cast<const uint8_t*>(m.pData);
        for (int y = 0; y < sh_; ++y) std::memcpy(base_.data() + size_t(y) * sw_ * 4, src + size_t(y) * m.RowPitch, size_t(sw_) * 4);
        ctx_->Unmap(staging_, 0);
        haveBase_ = true;
        return true;
    }

    // base_ + cursor -> out. If there's no new frame and the cursor didn't move, out stays as it was
    bool compose(Image& out, bool newFrame) {
        POINT c = cursorPos();
        bool moved = c.x != lastCursor_.x || c.y != lastCursor_.y;
        if (!newFrame && !moved && out.w == sw_ && out.h == sh_) { unchanged_ = true; return true; }
        lastCursor_ = c;
        if (!dib_.ensure(sw_, sh_)) return false;
        std::memcpy(dib_.bits(), base_.data(), base_.size());
        dib_.drawCursor(r_.left, r_.top);
        dib_.copyTo(out);
        return true;
    }

    void releaseDup() { if (dup_) { dup_->Release(); dup_ = nullptr; } }
    void releaseAll() {
        releaseDup();
        if (staging_) { staging_->Release(); staging_ = nullptr; }
        if (ctx_) { ctx_->Release(); ctx_ = nullptr; }
        if (dev_) { dev_->Release(); dev_ = nullptr; }
        sw_ = sh_ = 0;
    }

    HMONITOR mon_ = nullptr;
    RECT r_{};
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGIOutputDuplication* dup_ = nullptr;
    ID3D11Texture2D* staging_ = nullptr;
    int sw_ = 0, sh_ = 0;
    std::vector<uint8_t> base_;
    bool haveBase_ = false, unchanged_ = false;
    POINT lastCursor_{-1, -1};
    Dib dib_;
};

std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 1 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string exeOfWindow(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return "";
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::string out;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
        std::wstring full(buf, n);
        auto slash = full.find_last_of(L"\\/");
        out = narrow((slash == std::wstring::npos ? full : full.substr(slash + 1)).c_str());
    }
    CloseHandle(p);
    return out;
}

}  // namespace

std::vector<CaptureSource> ScreenCapturer::sources() {
    SetProcessDPIAware();
    std::vector<CaptureSource> v;
    auto mons = monitors();
    for (size_t i = 0; i < mons.size(); ++i) {
        const RECT& r = mons[i].r;
        v.push_back({"pantalla:" + std::to_string(i), T("Screen ") + std::to_string(i + 1) + " (" +
                                                          std::to_string(r.right - r.left) + "x" + std::to_string(r.bottom - r.top) +
                                                          (mons[i].primary ? T(", main)") : ")")});
    }
    struct Ctx { std::vector<CaptureSource>* v; };
    Ctx ctx{&v};
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(p);
        if (c->v->size() >= 60 || !IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER)) return TRUE;
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        // "Cloaked" by Windows even if they claim to be visible (suspended store apps, other desktops...)
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(h, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof cloaked)) && cloaked) return TRUE;
        wchar_t title[256];
        if (GetWindowTextW(h, title, 256) <= 0) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) return TRUE;  // not Eco's windows
        std::string t = narrow(title), exe = exeOfWindow(h);
        if (t == "Program Manager") return TRUE;
        char id[32];
        std::snprintf(id, sizeof id, "ventana:%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
        c->v->push_back({id, T("Window: ") + t + (exe.empty() ? "" : " (" + exe + ")")});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return v;
}

std::unique_ptr<ScreenCapturer> ScreenCapturer::create(const std::string& source) {
    SetProcessDPIAware();
    if (source.rfind("ventana:", 0) == 0) {
        auto h = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::strtoull(source.c_str() + 8, nullptr, 16)));
        if (IsWindow(h)) return std::make_unique<WindowCapturer>(h);
    }
    auto mons = monitors();
    size_t idx = 0;
    if (source.rfind("pantalla:", 0) == 0) idx = size_t(std::atoi(source.c_str() + 9));
    if (mons.empty()) {
        RECT r{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        return std::make_unique<GdiCapturer>(r);
    }
    if (idx >= mons.size()) idx = 0;
    // Diagnostics: ECO_CAPTURA_GDI=1 forces the old capture (in case DXGI gives problems on some computer)
    if (const char* g = std::getenv("ECO_CAPTURA_GDI"); g && *g == '1') return std::make_unique<GdiCapturer>(mons[idx].r);
    auto d = std::make_unique<DxgiCapturer>();
    if (d->init(mons[idx].h, mons[idx].r)) return d;
    return std::make_unique<GdiCapturer>(mons[idx].r);
}

namespace {

class Win32Viewer : public Viewer {
public:
    ~Win32Viewer() override { close(); }

    void open(const std::string& title) override {
        close();
        title_ = widen(title);
        running_ = true;
        thread_ = std::thread([this] { loop(); });
    }

    void close() override {
        running_ = false;
        HWND h = hwnd_.load();
        if (h) PostMessageW(h, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
    }

    bool isOpen() const override { return running_; }

    void present(const Image& img) override {
        {
            std::lock_guard<std::mutex> l(mu_);
            frame_ = img;
        }
        HWND h = hwnd_.load();
        if (h) InvalidateRect(h, nullptr, FALSE);
    }

private:
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Win32Viewer*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        switch (m) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                if (self) self->paint(h, dc);
                EndPaint(h, &ps);
                return 0;
            }
            case WM_CLOSE: DestroyWindow(h); return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(h, m, wp, lp);
    }

    void paint(HWND h, HDC dc) {
        RECT rc;
        GetClientRect(h, &rc);
        int ww = rc.right, wh = rc.bottom;
        // Double buffer to avoid flicker
        HDC back = CreateCompatibleDC(dc);
        HBITMAP bb = CreateCompatibleBitmap(dc, ww, wh);
        HGDIOBJ old = SelectObject(back, bb);
        FillRect(back, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        {
            std::lock_guard<std::mutex> l(mu_);
            if (frame_.w > 0 && ww > 0 && wh > 0) {
                double s = std::min(double(ww) / frame_.w, double(wh) / frame_.h);
                int dw = int(frame_.w * s), dh = int(frame_.h * s);
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = frame_.w;
                bi.bmiHeader.biHeight = -frame_.h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(back, HALFTONE);
                SetBrushOrgEx(back, 0, 0, nullptr);
                StretchDIBits(back, (ww - dw) / 2, (wh - dh) / 2, dw, dh, 0, 0, frame_.w, frame_.h,
                              frame_.bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            }
        }
        BitBlt(dc, 0, 0, ww, wh, back, 0, 0, SRCCOPY);
        SelectObject(back, old);
        DeleteObject(bb);
        DeleteDC(back);
    }

    void loop() {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"EcoViewer";
        RegisterClassW(&wc);  // fails harmlessly if it already exists
        HWND h = CreateWindowExW(0, L"EcoViewer", title_.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 1280, 760, nullptr, nullptr, inst, nullptr);
        if (!h) { running_ = false; return; }
        SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(this));
        hwnd_ = h;
        ShowWindow(h, SW_SHOW);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        hwnd_ = nullptr;
        running_ = false;
    }

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<HWND> hwnd_{nullptr};
    std::wstring title_;
    std::mutex mu_;
    Image frame_;
};

}  // namespace

bool globalKeyDown(const std::string& k) {
    int vk = 0;
    if (k.size() == 1 && std::isalnum((unsigned char)k[0])) vk = std::toupper((unsigned char)k[0]);
    else if (k.size() >= 2 && k.size() <= 3 && (k[0] == 'f' || k[0] == 'F') && std::isdigit((unsigned char)k[1])) {
        // atoi and not stoi: this runs on the push-to-talk thread and an exception there would close the program
        int n = std::atoi(k.c_str() + 1);
        if (n >= 1 && n <= 24) vk = VK_F1 + n - 1;
    }
    else if (k == "ctrl") vk = VK_CONTROL;
    else if (k == "alt") vk = VK_MENU;
    else if (k == "shift") vk = VK_SHIFT;
    else if (k == "capslock" || k == "mayus") vk = VK_CAPITAL;
    else if (k == "mouse4") vk = VK_XBUTTON1;
    else if (k == "mouse5") vk = VK_XBUTTON2;
    else if (k == "espacio" || k == "space") vk = VK_SPACE;
    if (!vk) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

std::unique_ptr<Viewer> Viewer::create() { return std::make_unique<Win32Viewer>(); }

}  // namespace eco
