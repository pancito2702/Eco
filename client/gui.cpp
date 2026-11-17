// eco-gui: graphical interface of the Eco client (Dear ImGui + GLFW; Direct3D 11 on Windows, OpenGL elsewhere).
// Works the same on Windows, Linux and macOS. Uses exactly the same client as the console
// (client.hpp): here we only draw and send orders. When idle it barely repaints, so it uses almost no CPU.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "../common/i18n.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "imgui_freetype.h"

#include "audio.hpp"
#include "client.hpp"
#include "commands.hpp"
#include "gui_render.hpp"
#include "keys.hpp"  // protectLocal: the server password saved with DPAPI
#include "../common/utf8args.hpp"
#include "../common/crash.hpp"
#include "../common/update.hpp"
#include "../common/version.hpp"
#include "loopback.hpp"
#include "screen.hpp"

#ifdef _WIN32
#include <shellapi.h>
#else
#include <spawn.h>
extern char** environ;
#endif

using namespace eco;

namespace {

GLFWwindow* g_win = nullptr;
std::atomic<bool> g_focused{true};
std::atomic<bool> g_attention{false};
int g_events = 0;  // keyboard and mouse events (to know whether to repaint now or wait)  // ask for attention on the taskbar (done on the main thread)

void requestQuit() { glfwSetWindowShouldClose(g_win, 1); }

// Opens a folder in the system file explorer
void openFolder(const std::string& dir) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, &w[0], n);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
    const char* tool = "open";
#else
    const char* tool = "xdg-open";
#endif
    char* argv[] = {const_cast<char*>(tool), const_cast<char*>(dir.c_str()), nullptr};
    pid_t pid;
    posix_spawnp(&pid, tool, nullptr, nullptr, argv, environ);
#endif
}

bool fileExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// ---------------------------------------------------------------- Screen being watched
// The network thread leaves the last frame here; the main thread uploads it to a texture
std::mutex frameMu;
Image frameImg;
bool frameNew = false;

void onScreenFrame(const Image& img) {
    {
        std::lock_guard<std::mutex> l(frameMu);
        frameImg = img;
        frameNew = true;
    }
    glfwPostEmptyEvent();  // wakes up the main loop
}

// ---------------------------------------------------------------- Ajustes guardados
// %APPDATA%\eco_gui.txt on Windows and ~/.eco_gui on Linux and macOS (never the folder it is opened from:
// the file may hold the server password)
std::string configPath() { return eco::configPath("eco_gui"); }

struct Settings {
    std::string server, nick, serverPass;
    float volume = 1.f, threshold = 0.012f;
    int bitrate = 40;
    std::string ptt;  // "" = voice detection
    int shareFps = 5, shareWidth = 1280;
    bool shareAudio = false;  // also share the PC audio
    std::string shareAudioApp;  // from which application ("" = the whole PC except Eco)
    float screenVolume = 1.f;   // volume of the audio of the screens you watch
    bool guideSeen = false;  // the quick guide shows up by itself the first time
    bool wizardDone = false; // the first-time assistant was already done (or skipped)

    void load() {
        std::ifstream f(configPath());
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (!v.empty() && v.back() == '\r') v.pop_back();
            try {
                if (k == "servidor") server = v;
                else if (k == "nick") nick = v;
                else if (k == "clave_servidor") serverPass = v;  // old format (plaintext): protected when saving
                else if (k == "clave_servidor_protegida") { std::string p; if (eco::unprotectLocal(v, p)) serverPass = p; }
                else if (k == "volumen") volume = std::stof(v);
                else if (k == "umbral") threshold = std::stof(v);
                else if (k == "calidad") bitrate = std::stoi(v);
                else if (k == "ptt") ptt = v;
                else if (k == "fps") shareFps = std::stoi(v);
                else if (k == "ancho") shareWidth = std::stoi(v);
                else if (k == "audio_pantalla") shareAudio = v == "1";
                else if (k == "audio_app") shareAudioApp = v;
                else if (k == "volumen_pantalla") screenVolume = std::stof(v);
                else if (k == "guia_vista") guideSeen = v == "1";
                else if (k == "asistente_visto") wizardDone = v == "1";
            } catch (...) {
            }
        }
    }
    void save() const {
        // The server password is protected with DPAPI (Windows); where there is no DPAPI, the file is private (0600)
        std::string pass;
        if (serverPass.empty()) pass = "clave_servidor=";
        else if (std::string sealed; eco::protectLocal(serverPass, sealed)) pass = "clave_servidor_protegida=" + sealed;
        else pass = "clave_servidor=" + serverPass;
#ifndef _WIN32
        { int fd = ::open(configPath().c_str(), O_WRONLY | O_CREAT, 0600); if (fd >= 0) ::close(fd); }
        ::chmod(configPath().c_str(), 0600);
#endif
        std::ofstream f(configPath(), std::ios::trunc);
        f << "servidor=" << server << "\nnick=" << nick << "\n" << pass << "\nvolumen=" << volume
          << "\numbral=" << threshold << "\ncalidad=" << bitrate << "\nptt=" << ptt << "\nfps=" << shareFps
          << "\nancho=" << shareWidth << "\naudio_pantalla=" << (shareAudio ? 1 : 0) << "\naudio_app=" << shareAudioApp
          << "\nvolumen_pantalla=" << screenVolume << "\nguia_vista=" << (guideSeen ? 1 : 0)
          << "\nasistente_visto=" << (wizardDone ? 1 : 0) << "\n";
    }
};

// ---------------------------------------------------------------- Log (chat and notices)
enum class Kind { Chat, Private, Info, Warn, Header, Block };

struct LogLine {
    std::string time, text;
    Kind kind;
    size_t nickLen = 0;  // in Chat/Private: how many characters at the start are "nick:"
    // Height it took when drawn at that width: the ones not visible are replaced by a gap of that height
    float height = 0, heightAt = -1;
};

std::mutex logMu;
std::vector<LogLine> logLines;
bool logDirty = false;

// ---------------------------------------------------------------- Text to display
// ImGui draws character by character: it doesn't combine emojis, doesn't order right-to-left text and doesn't
// join Arabic letters. Here each line is prepared only once, when it arrives (not on every frame).
namespace text {

std::u32string decode(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (i + n > s.size()) break;
        char32_t cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
        for (size_t k = 1; k < n; ++k) cp = cp << 6 | ((unsigned char)s[i + k] & 0x3F);
        out += cp;
        i += n;
    }
    return out;
}

std::string encode(const std::u32string& v) {
    std::string out;
    out.reserve(v.size() * 2);
    for (char32_t cp : v) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | cp >> 6); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += char(0xE0 | cp >> 12); out += char(0x80 | (cp >> 6 & 0x3F)); out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | cp >> 18); out += char(0x80 | (cp >> 12 & 0x3F));
            out += char(0x80 | (cp >> 6 & 0x3F)); out += char(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// What ImGui can't combine and would come out as loose little squares: emoji joiners (ZWJ),
// variation selectors, skin tones, the emoji keycap 1️⃣ and flag tags
bool dropForDisplay(char32_t c) {
    return c == 0x200D || c == 0xFE0E || c == 0xFE0F || c == 0x20E3 || (c >= 0x1F3FB && c <= 0x1F3FF) ||
           (c >= 0xE0020 && c <= 0xE007F);
}

bool isRtl(char32_t c) {
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}
bool isArabicDigit(char32_t c) { return (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9); }
bool isMark(char32_t c);
// Only letters open a right-to-left run; Arabic digits don't (٩(◕‿◕)۶ stays the same)
bool isStrongRtl(char32_t c) { return isRtl(c) && !isArabicDigit(c) && !isMark(c); }
bool isMark(char32_t c) {  // marks that go above/below the previous letter
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0591 && c <= 0x05C7) || (c >= 0x0610 && c <= 0x061A) ||
           (c >= 0x064B && c <= 0x065F) || c == 0x0670 || (c >= 0x06D6 && c <= 0x06ED);
}
bool isStrongLtr(char32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c < 0x0590 && c != 0xD7 && c != 0xF7 &&
                                                                 !isMark(c)) ||
           (c >= 0x0900 && c < 0x2000) || (c >= 0x3040 && c < 0xFB1D) || c >= 0x10000;
}

// Contextual forms of Arabic ("Presentation Forms-B" block): base = isolated form;
// +1 final, +2 initial, +3 medial. dual = joins on both sides.
struct ArabicForm { uint16_t base; bool dual; };
const ArabicForm* arabicForm(char32_t c) {
    static const ArabicForm t[] = {
        {0xFE80, false}, {0xFE81, false}, {0xFE83, false}, {0xFE85, false}, {0xFE87, false}, {0xFE89, true},   // 0621-0626
        {0xFE8D, false}, {0xFE8F, true},  {0xFE93, false}, {0xFE95, true},  {0xFE99, true},  {0xFE9D, true},   // 0627-062C
        {0xFEA1, true},  {0xFEA5, true},  {0xFEA9, false}, {0xFEAB, false}, {0xFEAD, false}, {0xFEAF, false},  // 062D-0632
        {0xFEB1, true},  {0xFEB5, true},  {0xFEB9, true},  {0xFEBD, true},  {0xFEC1, true},  {0xFEC5, true},   // 0633-0638
        {0xFEC9, true},  {0xFECD, true}};                                                                     // 0639-063A
    static const ArabicForm u[] = {
        {0xFED1, true}, {0xFED5, true}, {0xFED9, true}, {0xFEDD, true}, {0xFEE1, true}, {0xFEE5, true},  // 0641-0646
        {0xFEE9, true}, {0xFEED, false}, {0xFEEF, false}, {0xFEF1, true}};                               // 0647-064A
    if (c >= 0x0621 && c <= 0x063A) return &t[c - 0x0621];
    if (c >= 0x0641 && c <= 0x064A) return &u[c - 0x0641];
    return nullptr;
}

void shapeArabic(std::u32string& v) {
    auto joinsNext = [&](size_t i) {  // does letter i join the next one (in logical order)?
        if (v[i] == 0x0640) return true;  // tatweel
        auto f = arabicForm(v[i]);
        return f && f->dual;
    };
    auto joinsPrev = [&](size_t i) { return v[i] == 0x0640 || arabicForm(v[i]) != nullptr; };
    std::u32string out = v;
    for (size_t i = 0; i < v.size(); ++i) {
        auto f = arabicForm(v[i]);
        if (!f || v[i] == 0x0621) continue;
        size_t p = i, n = i;  // neighbours skipping the marks (harakat)
        bool prev = false, next = false;
        while (p > 0) { --p; if (!isMark(v[p])) { prev = joinsNext(p); break; } }
        while (++n < v.size()) if (!isMark(v[n])) { next = joinsPrev(n); break; }
        if (!f->dual) next = false;
        out[i] = f->base + (prev && next ? 3 : prev ? 1 : next ? 2 : 0);
    }
    v.swap(out);
}

char32_t mirror(char32_t c) {
    switch (c) {
        case '(': return ')'; case ')': return '(';
        case '[': return ']'; case ']': return '[';
        case '{': return '}'; case '}': return '{';
        case '<': return '>'; case '>': return '<';
        case 0xAB: return 0xBB; case 0xBB: return 0xAB;
        default: return c;
    }
}

// Simple visual order (left-to-right paragraph, like the "nick: " prefix): each run of
// RTL text is reversed, keeping each letter with its marks and numbers in their order
void reorderRtl(std::u32string& v) {
    size_t i = 0;
    while (i < v.size()) {
        if (!isStrongRtl(v[i])) { ++i; continue; }
        // the RTL run continues as long as no LTR letter appears (spaces and signs in between, inside)
        size_t end = i, lastRtl = i;
        for (size_t k = i; k < v.size() && !isStrongLtr(v[k]); ++k)
            if (isStrongRtl(v[k]) || (isMark(v[k]) && k == lastRtl + 1)) lastRtl = k;
        end = lastRtl + 1;
        // units: letter + marks, or a whole number
        std::vector<std::u32string> units;
        for (size_t k = i; k < end;) {
            size_t j = k + 1;
            auto digit = [](char32_t c) {
                return (c >= '0' && c <= '9') || (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9);
            };
            if (digit(v[k])) while (j < end && (digit(v[j]) || v[j] == '.' || v[j] == ',')) ++j;
            else while (j < end && isMark(v[j])) ++j;
            std::u32string u = v.substr(k, j - k);
            if (u.size() == 1) u[0] = mirror(u[0]);
            units.push_back(std::move(u));
            k = j;
        }
        std::u32string rev;
        for (auto it = units.rbegin(); it != units.rend(); ++it) rev += *it;
        v.replace(i, end - i, rev);
        i = end;
    }
}

std::string forDisplay(const std::string& s) {
    // Fast path: almost all lines are ASCII or Latin and there is nothing to do
    bool special = false;
    for (unsigned char c : s)
        if (c >= 0xD6 || c == 0xE2) { special = true; break; }  // >= U+0580 or U+2000-2FFF
    if (!special) return s;
    std::u32string v = decode(s);
    v.erase(std::remove_if(v.begin(), v.end(), dropForDisplay), v.end());
    if (std::any_of(v.begin(), v.end(), isStrongRtl)) {
        shapeArabic(v);
        reorderRtl(v);
    }
    return encode(v);
}

}  // namespace text

void addLog(const std::string& s) {
    LogLine l;
    auto t = std::time(nullptr);
    char b[16];
    std::strftime(b, sizeof b, "%H:%M", std::localtime(&t));
    l.time = b;
    l.text = text::forDisplay(s);
    if (s.find('\n') != std::string::npos) l.kind = Kind::Block;
    else if (s.rfind("! ", 0) == 0) l.kind = Kind::Warn;
    else if (s.rfind("* ", 0) == 0) l.kind = Kind::Info;
    else if (s.rfind("== ", 0) == 0) l.kind = Kind::Header;
    else if (s.rfind("[privado", 0) == 0 || s.rfind("[private", 0) == 0) l.kind = Kind::Private;
    else {
        l.kind = Kind::Chat;
        auto c = s.find(": ");
        if (c != std::string::npos && c < 40 && s.substr(0, c).find(' ') == std::string::npos) l.nickLen = c + 1;
        else l.kind = Kind::Info;
    }
    if (l.kind == Kind::Private) {
        auto c = s.find(": ");
        if (c != std::string::npos) l.nickLen = c + 1;
        if (!g_focused) g_attention = true;  // signal on the taskbar
    }
    std::lock_guard<std::mutex> lk(logMu);
    logLines.push_back(std::move(l));
    if (logLines.size() > 3000) logLines.erase(logLines.begin(), logLines.begin() + 500);
    logDirty = true;
    glfwPostEmptyEvent();  // show it right away, even if the window is idle
}

// ---------------------------------------------------------------- Colores
const ImVec4 kAccent(0.30f, 0.72f, 0.66f, 1.f);
const ImVec4 kDim(0.55f, 0.58f, 0.62f, 1.f);
const ImVec4 kWarn(0.95f, 0.55f, 0.35f, 1.f);
const ImVec4 kPriv(0.78f, 0.62f, 0.98f, 1.f);
const ImVec4 kText(0.90f, 0.91f, 0.93f, 1.f);
const ImVec4 kTalk(0.30f, 0.85f, 0.45f, 1.f);
const ImVec4 kMuted(0.90f, 0.35f, 0.35f, 1.f);
const ImVec4 kAdmin(0.98f, 0.75f, 0.30f, 1.f);
const ImVec4 kMod(0.45f, 0.70f, 0.98f, 1.f);

ImVec4 nickColor(const std::string& nick) {
    // A stable color per nick, readable on a dark background
    uint32_t h = 2166136261u;
    for (char c : nick) h = (h ^ uint8_t(std::tolower((unsigned char)c))) * 16777619u;
    float hue = (h % 360) / 360.f, r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.45f, 0.95f, r, g, b);
    return ImVec4(r, g, b, 1.f);
}

void applyTheme(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 8;
    st.ChildRounding = 8;
    st.FrameRounding = 6;
    st.PopupRounding = 6;
    st.GrabRounding = 6;
    st.TabRounding = 6;
    st.ScrollbarRounding = 8;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(8, 6);
    st.WindowPadding = ImVec2(10, 10);
    st.ScrollbarSize = 12;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.105f, 0.115f, 0.13f, 1.f);
    c[ImGuiCol_ChildBg] = ImVec4(0.135f, 0.145f, 0.165f, 1.f);
    c[ImGuiCol_PopupBg] = ImVec4(0.15f, 0.16f, 0.18f, 0.98f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.09f, 0.10f, 0.11f, 1.f);
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_FrameBg] = ImVec4(0.19f, 0.20f, 0.23f, 1.f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.23f, 0.25f, 0.28f, 1.f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.28f, 0.32f, 1.f);
    c[ImGuiCol_Button] = ImVec4(0.21f, 0.23f, 0.26f, 1.f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.30f, 0.34f, 1.f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.45f, 0.42f, 1.f);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.36f, 0.35f, 0.8f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.30f, 0.32f, 1.f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.45f, 0.42f, 1.f);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.85f, 0.78f, 1.f);
    c[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.10f, 0.11f, 1.f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.14f, 0.16f, 1.f);
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.24f, 0.27f, 1.f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    st.ScaleAllSizes(scale);
}

// Button that looks "on" when on is true
bool toggleButton(const char* label, bool on, const ImVec4& onColor, const ImVec2& size = ImVec2(0, 0)) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(onColor.x * 0.55f, onColor.y * 0.55f, onColor.z * 0.55f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(onColor.x * 0.68f, onColor.y * 0.68f, onColor.z * 0.68f, 1.f));
    }
    bool r = ImGui::Button(label, size);
    if (on) ImGui::PopStyleColor(2);
    return r;
}

// Mic level bar with the threshold mark
void levelMeter(float level, float threshold, const ImVec2& size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float full = 0.1f;  // RMS 0.1 = full bar (normal voice is around 0.02-0.08)
    float lv = std::min(1.f, level / full), th = std::min(1.f, threshold / full);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(40, 44, 50, 255), size.y * 0.5f);
    ImU32 col = level >= threshold ? ImGui::GetColorU32(kTalk) : IM_COL32(110, 118, 128, 255);
    if (lv > 0.01f) dl->AddRectFilled(p, ImVec2(p.x + size.x * lv, p.y + size.y), col, size.y * 0.5f);
    float tx = p.x + size.x * th;
    dl->AddLine(ImVec2(tx, p.y - 2), ImVec2(tx, p.y + size.y + 2), ImGui::GetColorU32(kWarn), 2.f);
    ImGui::Dummy(size);
}

// ---------------------------------------------------------------- Application
class App {
public:
    explicit App(float scale) : scale_(scale) {
        cfg_.load();
        server_ = cfg_.server;
        nick_ = cfg_.nick;
        serverPass_ = cfg_.serverPass;
        // Really the first time (nothing saved): the assistant shows up by itself
        openWizard_ = !cfg_.wizardDone && cfg_.nick.empty() && cfg_.server.empty();
        crashReport_ = crash::unseenReport(crash::clientDir());  // did it close badly last time?
        openCrash_ = !crashReport_.empty();
    }

    // eco-gui <server> <nick>: fills in and connects without going through the form
    void autoConnect(const std::string& server, const std::string& nick) {
        openWizard_ = false;  // with server and name on the command line it is not needed
        server_ = server;
        nick_ = nick;
        startConnect();
    }

    ~App() {
        disconnect();
    }

    void frame() {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (screen_ == Screen::Main) f |= ImGuiWindowFlags_MenuBar;
        ImGui::Begin("##root", nullptr, f);
        pollConnect();
        if (screen_ == Screen::Main && client_ && !client_->alive()) {
            // The connection dropped: if it wasn't a kick or a change of the server identity, it retries by itself
            bool retry = wasJoined_ && client_->retryable() && client_->hostKeyChanged().empty();
            loginError_ = client_->error();
            if (loginError_.empty()) loginError_ = T("Lost the connection to the server");
            disconnect();
            if (retry) scheduleReconnect(true);
        }
        if (reconnecting_ && screen_ == Screen::Login && std::chrono::steady_clock::now() >= reconnectAt_) startConnect(true);
        if (screen_ == Screen::Main) mainScreen();
        else loginScreen();
        if (screen_ != Screen::Main) wizardModal();
        crashModal();
        ImGui::End();
    }

    // Is anything moving that asks for frequent repaints? (if not, it repaints little to save CPU)
    // How often to repaint even if nothing happens (seconds). Messages, screen frames,
    // the mouse and the keyboard wake the window up at once; this only sets the pace of animations.
    double idleWait(bool focused) const {
        if (wizAudio_) return 1.0 / 30;  // the assistant's mic meter
        if (screen_ == Screen::Connecting || reconnecting_) return 0.1;
        if (screen_ != Screen::Main) return focused ? 0.5 : 2.0;
        if (showSettings_) return 1.0 / 30;  // smooth mic meter while adjusting
        if (anyTalking_) return 0.1;         // indicator of who is talking: 10 fps is plenty
        return focused ? 0.25 : 0.5;
    }

private:
    enum class Screen { Login, Connecting, Main };

    float px(float v) const { return v * scale_; }

    // ---------------------------------------------------------- Connection
    // Retries when the connection is lost: 1, 2, 4, 8, 16 and then every 30 s
    void scheduleReconnect(bool first) {
        if (first) reconnectTry_ = 0;
        int delay = std::min(30, 1 << std::min(reconnectTry_, 5));
        ++reconnectTry_;
        reconnecting_ = true;
        reconnectAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(delay);
    }

    void startConnect(bool reconnect = false) {
        if (!reconnect) reconnecting_ = false;
        loginError_.clear();
        ClientOptions o;
        std::string remembered = server_;  // what is saved (without the server password, which goes separately and protected)
        if (isInvite(server_)) {
            Invite inv;
            if (!parseInvite(server_, inv)) {
                loginError_ = T("The invitation is not valid. Copy all of it, from eco:// to the end.");
                return;
            }
            o.host = inv.host;
            o.port = inv.port;
            o.expectedFingerprint = inv.fingerprint;
            if (!inv.serverPass.empty()) serverPass_ = inv.serverPass;
            if (inv.host == "TU-IP-PUBLICA") {
                loginError_ = T("The invitation is missing the address: replace TU-IP-PUBLICA with the server's IP or domain.");
                return;
            }
            remembered = makeInvite(inv.host, inv.port, inv.fingerprint, "");
        } else if (!parseHostPort(server_, o.host, o.port)) {
            loginError_ = T("Invalid address. Paste the invitation you were given (it starts with eco://) or type the "
                            "address, for example 192.168.1.10");
            return;
        }
        if (nick_.empty() || nick_.find(' ') != std::string::npos) {
            loginError_ = T("Type your name (no spaces, for example John or John_S)");
            return;
        }
        server_ = remembered;  // this way the server password isn't visible in the field or in the file
        o.nick = nick_;
        o.accountPass = accountPass_;
        o.serverPass = serverPass_;
        o.trustNewKey = trustNext_;
        o.onScreenFrame = onScreenFrame;
        if (reconnect) o.rejoinChannel = lastChannel_;
        trustNext_ = false;
        newHostKey_.clear();
        cfg_.server = server_;
        cfg_.nick = nick_;
        cfg_.serverPass = serverPass_;
        cfg_.save();
        hostLabel_ = (o.host.find(':') != std::string::npos ? "[" + o.host + "]" : o.host) +
                     (o.port != kDefaultPort ? ":" + std::to_string(o.port) : "");
        if (!reconnect) {  // when reconnecting, what was in the chat is kept
            std::lock_guard<std::mutex> l(logMu);
            logLines.clear();
        }
        client_ = EcoClient::create(o);
        EcoClient* c = client_.get();
        connecting_ = std::async(std::launch::async, [c] { return c->connect(); });
        screen_ = Screen::Connecting;
    }

    void pollConnect() {
        if (screen_ != Screen::Connecting || !connecting_.valid()) return;
        if (connecting_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        bool ok = connecting_.get();
        if (ok) {
            reconnecting_ = false;
            reconnectTry_ = 0;
            screen_ = Screen::Main;
            applied_ = false;
            authHandled_ = 0;
            focusInput_ = true;
            return;
        }
        loginError_ = client_->error();
        newHostKey_ = client_->hostKeyChanged();
        if (loginError_.empty()) loginError_ = T("Could not connect");
        bool again = reconnecting_ && newHostKey_.empty() && client_->retryable();
        client_.reset();
        screen_ = Screen::Login;
        if (again) scheduleReconnect(false);
        else reconnecting_ = false;
    }

    void reconnectPanel() {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float w = std::min(px(460), avail.x - px(20));
        ImGui::SetCursorPos(ImVec2((avail.x - w) * 0.5f + px(10), std::max(px(20), avail.y * 0.25f)));
        ImGui::BeginChild("reconnect", ImVec2(w, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::TextColored(kWarn, T("Lost the connection to %s"), hostLabel_.c_str());
        ImGui::PushTextWrapPos(0);
        if (!loginError_.empty()) ImGui::TextDisabled("%s", loginError_.c_str());
        ImGui::PopTextWrapPos();
        if (screen_ == Screen::Connecting) {
            ImGui::Text(T("Reconnecting... (attempt %d)"), reconnectTry_);
        } else {
            auto left = std::chrono::duration_cast<std::chrono::seconds>(reconnectAt_ - std::chrono::steady_clock::now()).count();
            ImGui::Text(T("Trying again in %lld s (attempt %d)"), static_cast<long long>(std::max<long long>(0, left) + 1),
                        reconnectTry_ + 1);
            if (ImGui::Button(T("Retry now"))) reconnectAt_ = std::chrono::steady_clock::now();
            ImGui::SameLine();
        }
        if (ImGui::Button(T("Cancel"))) {
            reconnecting_ = false;
            if (screen_ == Screen::Connecting) disconnect();
        }
        ImGui::EndChild();
    }

    // Last time Eco closed unexpectedly: warn once and say where the report is
    void crashModal() {
        if (openCrash_) {
            ImGui::OpenPopup(T("Eco closed unexpectedly"));
            openCrash_ = false;
        }
        if (!ImGui::BeginPopupModal(T("Eco closed unexpectedly"), nullptr, ImGuiWindowFlags_NoResize)) return;
        ImGui::PushTextWrapPos(px(440));
        ImGui::TextUnformatted(T("Last time Eco closed by accident. A report was saved that only says at which point of the "
                                 "program it failed: it has no messages, names, passwords or anything personal, and it has not "
                                 "been sent to anyone."));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", crashReport_.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(T("If you want to help fix it, send it to whoever gave you Eco."));
        ImGui::PopTextWrapPos();
        if (ImGui::Button(T("Open the folder"))) openFolder(crash::clientDir());
        ImGui::SameLine();
        if (ImGui::Button(T("Close"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void disconnect() {
        if (connecting_.valid()) connecting_.wait();
        if (client_) client_->shutdown();
        client_.reset();
        screen_ = Screen::Login;
        showSettings_ = false;
    }

    // Saved settings applied when entering the channel (the client rejects /ptt before)
    void applySettings() {
        client_->setVolume(cfg_.volume);
        client_->setThreshold(cfg_.threshold);
        client_->setBitrate(cfg_.bitrate);
        if (!cfg_.ptt.empty()) client_->setPushToTalk(cfg_.ptt);
        client_->setShareAudio(cfg_.shareAudio);
        client_->setShareAudioApp(cfg_.shareAudioApp);
        client_->setScreenVolume(cfg_.screenVolume);
        client_->requestDevices();
        applied_ = true;
    }

    // ---------------------------------------------------------- Login screen
    void loginScreen() {
        if (reconnecting_) return reconnectPanel();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float w = std::min(px(460), avail.x - px(20));
        ImGui::SetCursorPos(ImVec2((avail.x - w) * 0.5f + px(10), std::max(px(20), avail.y * 0.12f)));
        ImGui::BeginChild("login", ImVec2(w, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.9f);  // sharp, not a stretched bitmap
        ImGui::TextUnformatted(T("Eco"));
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::TextDisabled(T("Lightweight, end-to-end encrypted voice chat · version %s"), eco::kVersion);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool busy = screen_ == Screen::Connecting;
        bool enter = false;
        ImGui::BeginDisabled(busy);
        ImGui::TextUnformatted(T("Server or invitation"));
        ImGui::SameLine();
        ImGui::TextDisabled(T("(paste the invitation you were given)"));
        ImGui::SetNextItemWidth(-px(70));
        enter |= ImGui::InputTextWithHint("##srv", T("eco://...   or   192.168.1.10"), &server_,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button(T("Paste"), ImVec2(-1, 0))) {
            if (const char* clip = ImGui::GetClipboardText()) {
                server_ = clip;
                while (!server_.empty() && std::isspace((unsigned char)server_.back())) server_.pop_back();
            }
        }
        if (isInvite(server_)) {
            Invite inv;
            if (parseInvite(server_, inv))
                ImGui::TextColored(kTalk, T("Invitation to %s:%u. The server's identity will be checked automatically."), inv.host.c_str(),
                                   unsigned(inv.port));
            else
                ImGui::TextColored(kWarn, T("The invitation looks incomplete: copy all of it."));
        }
        ImGui::TextUnformatted(T("Your name"));
        ImGui::SetNextItemWidth(-1);
        enter |= ImGui::InputTextWithHint("##nick", T("How others will see you"), &nick_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextUnformatted(T("Your account password"));
        ImGui::SameLine();
        ImGui::TextDisabled(T("(only if you reserved your name)"));
        ImGui::SetNextItemWidth(-1);
        enter |= ImGui::InputTextWithHint("##pass", T("optional"), &accountPass_,
                                          ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::TreeNode(T("More options"))) {
            ImGui::TextUnformatted(T("Server password"));
            ImGui::SameLine();
            ImGui::TextDisabled(T("(if the server uses --pass)"));
            ImGui::SetNextItemWidth(-1);
            enter |= ImGui::InputTextWithHint("##spass", T("optional"), &serverPass_,
                                              ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::TreePop();
        }
        ImGui::Spacing();
        bool go = ImGui::Button(busy ? T("Connecting...") : T("Connect"), ImVec2(-1, px(36)));
        ImGui::EndDisabled();
        if ((go || enter) && !busy) startConnect();
        if (!busy) {
            ImGui::Spacing();
            ImGui::TextDisabled(T("First time?"));
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Open the setup assistant"))) openWizard_ = true;
        }

        if (!newHostKey_.empty() && !busy) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.30f, 0.14f, 0.12f, 1.f));
            ImGui::BeginChild("hostkey", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(kWarn, "%s", T("SECURITY WARNING"));
            ImGui::TextUnformatted(
                T("This server's identity has changed since last time. Someone could be impersonating it to spy "
                  "on the connection."));
            ImGui::TextUnformatted(T("Fingerprint it shows now:"));
            ImGui::TextColored(kAdmin, "%s", newHostKey_.c_str());
            ImGui::TextDisabled(T("If the owner reinstalled the server or deleted eco.db, check this fingerprint with them before "
                                  "trusting it."));
            ImGui::PopTextWrapPos();
            if (ImGui::Button(T("I checked the fingerprint: trust and connect"))) {
                trustNext_ = true;
                startConnect();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        } else if (!loginError_.empty() && !busy) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(kWarn, "%s", loginError_.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
    }

    // ---------------------------------------------------------- Pantalla principal
    void mainScreen() {
        UiState s = client_->state();
        if (s.joined && !applied_) applySettings();
        if (s.joined) {
            wasJoined_ = true;
            lastChannel_ = s.channel;
        }
        anyTalking_ = s.talking || std::any_of(s.users.begin(), s.users.end(), [](const UiUser& u) { return u.talking; });
        isMod_ = s.role != Role::User;
        isAdmin_ = s.role == Role::Admin;

        menuBar(s);
        toolbar(s);
        updateBar(s);

        float footer = ImGui::GetFrameHeightWithSpacing();
        float h = ImGui::GetContentRegionAvail().y - footer;
        float leftW = std::max(px(220), std::min(px(320), ImGui::GetContentRegionAvail().x * 0.3f));
        ImGui::BeginChild("tree", ImVec2(leftW, h), ImGuiChildFlags_AlwaysUseWindowPadding);
        channelTree(s);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("chatcol", ImVec2(0, h), ImGuiChildFlags_None);
        chatPanel(s);
        ImGui::EndChild();
        statusBar(s);

        passwordModal(s);
        createChannelModal();
        if (s.joined && !cfg_.guideSeen) {  // first time: the guide by itself, only once
            cfg_.guideSeen = true;
            cfg_.save();
            openGuide_ = true;
        }
        verifyModal(s);
        inviteModal(s);
        accountPassModal();
        guideModal();
        aboutModal(s);
        if (showSettings_) settingsWindow(s);
        if (s.watching) screenWindow(s);
    }

    // Which application the audio shared with the screen comes from
    void audioAppCombo() {
        std::string cur = cfg_.shareAudioApp.empty() ? T("Everything playing on the PC (except Eco)") : cfg_.shareAudioApp;
        ImGui::SetNextItemWidth(px(320));
        if (ImGui::BeginCombo(T("Audio"), cur.c_str())) {
            auto pick = [&](const std::string& exe) {
                cfg_.shareAudioApp = exe;
                client_->setShareAudioApp(exe);
                cfg_.save();
            };
            if (ImGui::Selectable(T("Everything playing on the PC (except Eco)"), cfg_.shareAudioApp.empty())) pick("");
            bool listed = cfg_.shareAudioApp.empty();
            for (auto& a : audioApps_) {
                listed |= a.exe == cfg_.shareAudioApp;
                if (ImGui::Selectable((T("Only ") + a.label).c_str(), a.exe == cfg_.shareAudioApp)) pick(a.exe);
            }
            if (!listed) ImGui::Selectable((T("Only ") + cfg_.shareAudioApp + T(" (no sound right now)")).c_str(), true);
            if (audioApps_.empty()) ImGui::TextDisabled(T("(no application has sound open right now)"));
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(T("Choosing an application keeps out notifications, other calls or your music (Windows 10 2004+)"));
    }

    // New version offered by the server (signed): install, progress, restart
    void updateBar(const UiState& s) {
        if (s.updateReady) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kTalk, T("Eco %s is installed."), s.updateVersion.c_str());
            ImGui::SameLine();
            if (ImGui::Button(T("Restart now"))) {
                if (update::relaunch()) requestQuit();
            }
        } else if (s.updateProgress >= 0) {
            std::string t = T("Downloading Eco ") + s.updateVersion + "...";
            ImGui::ProgressBar(s.updateProgress, ImVec2(px(320), 0), t.c_str());
        } else if (!s.updateVersion.empty()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kAccent, T("There is a new version of Eco: %s (yours is %s)."), s.updateVersion.c_str(),
                               kVersion);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(T("It is offered by this server and signed by Eco's publisher: the signature has already been checked."));
            ImGui::SameLine();
            if (ImGui::Button(T("Install"))) client_->startUpdate();
        }
    }

    void aboutModal(const UiState& s) {
        if (openAbout_) {
            ImGui::OpenPopup(T("About Eco"));
            openAbout_ = false;
        }
        if (!ImGui::BeginPopupModal(T("About Eco"), nullptr, ImGuiWindowFlags_NoResize)) return;
        ImGui::PushTextWrapPos(px(480));
        ImGui::TextColored(kAccent, T("Eco %s"), kVersion);
        ImGui::TextUnformatted(T("Lightweight, end-to-end encrypted voice chat."));
        ImGui::TextDisabled(T("Server: %s · protocol %u · %s"), s.serverVersion.empty() ? T("older than 2.1") : s.serverVersion.c_str(),
                            unsigned(kProtoVersion), update::platform());
        ImGui::SeparatorText(T("Privacy"));
        ImGui::TextUnformatted(
            T("Chat, voice, screen and its audio are end-to-end encrypted: not even the server can read or "
              "hear them. The server sees who is connected, in which channel and when each person talks, and "
              "stores the encrypted history messages. On this computer, Eco keeps your settings and your keys "
              "(protected with your Windows user or readable only by you). Eco sends nothing anywhere other "
              "than your server: no statistics, no crash reports (those stay on your computer)."));
        ImGui::SeparatorText(T("Third-party software"));
        ImGui::TextUnformatted(
            T("Monocypher (CC0 / BSD-2) · SQLite (public domain) · stb_image (MIT / public domain) · Opus "
              "(BSD-3, Xiph) · RNNoise (BSD-3, Xiph) · PortAudio (MIT) · Asio (Boost Software License 1.0) · "
              "Dear ImGui (MIT) · GLFW (zlib) · FreeType (FreeType License)."));
        ImGui::TextDisabled(T("Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights "
                              "reserved."));
        ImGui::TextDisabled(T("The full license texts are in THIRD_PARTY.md, next to Eco."));
        ImGui::PopTextWrapPos();
        if (ImGui::Button(T("Close"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---------------------------------------------------------- Screen viewer (inside the GUI)
    void uploadFrame() {
        std::lock_guard<std::mutex> l(frameMu);
        if (!frameNew || frameImg.w <= 0) return;
        frameNew = false;
        tex_ = render::updateTexture(frameImg);
        texW_ = frameImg.w;
        texH_ = frameImg.h;
    }

    void screenWindow(const UiState& s) {
        if (watchingId_ != s.watching) {  // another person: don't show the last frame of the previous one
            watchingId_ = s.watching;
            texW_ = texH_ = 0;
            std::lock_guard<std::mutex> l(frameMu);
            frameNew = false;
        }
        uploadFrame();
        std::string who = T("someone");
        for (auto& u : s.users)
            if (u.id == s.watching) who = u.nick;
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.7f, vp->WorkSize.y * 0.7f), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bool open = true;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        std::string title = T("Screen: ") + who + "###screen";
        bool hasAudio = false;
        for (auto& u : s.users)
            if (u.id == s.watching) hasAudio = u.screenAudio;
        if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                                   ImGuiWindowFlags_NoScrollbar)) {
            if (hasAudio) {  // their screen carries their PC audio: volume separate from the voices
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(6), px(3)));
                ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + px(8), ImGui::GetCursorPosY() + px(4)));
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled(T("Audio from their PC"));
                ImGui::SameLine();
                int v = int(cfg_.screenVolume * 100 + 0.5f);
                ImGui::SetNextItemWidth(px(200));
                if (ImGui::SliderInt("##volpantalla", &v, 0, 200, "%d %%")) {
                    cfg_.screenVolume = v / 100.f;
                    client_->setScreenVolume(cfg_.screenVolume);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) cfg_.save();
                ImGui::PopStyleVar();
                ImGui::Dummy(ImVec2(0, px(2)));
            }
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + avail.x, p.y + avail.y), IM_COL32(0, 0, 0, 255));
            if (tex_ && texW_ > 0 && avail.x > 0 && avail.y > 0) {
                float k = std::min(avail.x / texW_, avail.y / texH_);
                ImVec2 sz(texW_ * k, texH_ * k);
                ImVec2 a(p.x + (avail.x - sz.x) * 0.5f, p.y + (avail.y - sz.y) * 0.5f);
                dl->AddImage(tex_, a, ImVec2(a.x + sz.x, a.y + sz.y));
            } else {
                const char* t = T("Waiting for the image...");
                ImVec2 ts = ImGui::CalcTextSize(t);
                dl->AddText(ImVec2(p.x + (avail.x - ts.x) * 0.5f, p.y + (avail.y - ts.y) * 0.5f), ImGui::GetColorU32(kDim), t);
            }
            ImGui::Dummy(avail);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        if (!open) client_->submit("/unwatch");
    }

    void menuBar(const UiState& s) {
        if (!ImGui::BeginMenuBar()) return;
        if (ImGui::BeginMenu(T("Eco"))) {
            if (ImGui::MenuItem(T("Invite a friend..."))) openInvite_ = true;
            if (ImGui::MenuItem(T("Settings..."))) openSettings();
            if (ImGui::MenuItem(T("Disconnect"))) {
                loginError_.clear();
                disconnect();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(T("Quit"))) requestQuit();
            ImGui::Separator();
            if (!s.updateVersion.empty() && !s.updateReady && s.updateProgress < 0) {
                std::string u = T("Install Eco ") + s.updateVersion;
                if (ImGui::MenuItem(u.c_str())) client_->startUpdate();
            }
            if (ImGui::MenuItem(T("About Eco..."))) openAbout_ = true;
            std::string ver = std::string(T("Eco ")) + eco::kVersion + T(" · server ") +
                              (s.serverVersion.empty() ? std::string(T("older than 2.1")) : s.serverVersion);
            ImGui::MenuItem(ver.c_str(), nullptr, false, false);  // informational only
            ImGui::EndMenu();
        }
        if (!client_) { ImGui::EndMenuBar(); return; }
        if (ImGui::BeginMenu(T("View"))) {
            if (ImGui::MenuItem(T("Channel history (100)"))) client_->submit("/history 100");
            if (ImGui::MenuItem(T("My private messages"))) client_->submit("/pms 50");
            if (ImGui::MenuItem(T("Network statistics"))) client_->submit("/stats");
            if (ImGui::MenuItem(T("Audio devices"))) client_->submit("/devices");
            ImGui::Separator();
            if (ImGui::MenuItem(T("Clear chat"))) {
                std::lock_guard<std::mutex> l(logMu);
                logLines.clear();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Account"))) {
            // Passwords are asked for in windows with hidden text, never in the chat box
            if (!s.registered && ImGui::MenuItem(T("Reserve my name (create account)..."))) openPass_ = PassMode::Register;
            if (s.registered && ImGui::MenuItem(T("Change password..."))) openPass_ = PassMode::Change;
            if (ImGui::MenuItem(T("Become mod/admin with a password..."))) openPass_ = PassMode::Login;
            ImGui::Separator();
            if (ImGui::MenuItem(T("Shared E2E phrase..."))) openSettings();
            ImGui::EndMenu();
        }
        if (isMod_ && ImGui::BeginMenu(isAdmin_ ? T("Admin") : T("Moderation"))) {
            if (ImGui::MenuItem(T("Create channel..."))) openCreateChannel_ = true;
            if (isAdmin_) {
                ImGui::Separator();
                if (ImGui::MenuItem(T("Registered accounts"))) client_->submit("/accounts");
                if (ImGui::MenuItem(T("Banned"))) client_->submit("/bans");
                if (ImGui::MenuItem(T("Unban IP..."))) prefill(slashCommand("unban") + " ");
                if (ImGui::MenuItem(T("Delete account..."))) prefill(slashCommand("deleteaccount") + " ");
                if (ImGui::MenuItem(T("Reset account password..."))) prefill(slashCommand("resetpassword") + " ");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Help"))) {
            if (ImGui::MenuItem(T("Quick guide"))) openGuide_ = true;
            if (ImGui::MenuItem(T("Commands (advanced)"))) client_->submit("/help");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void toolbar(const UiState& s) {
        ImVec2 bs(0, px(32));
        float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        if (toggleButton(s.muted ? T("Mic muted") : T("Mic on"), s.muted, kMuted, bs)) client_->setMuted(!s.muted);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("Mute / unmute your microphone"));
        ImGui::SameLine();
        if (toggleButton(s.deafened ? T("Sound off") : T("Sound on"), s.deafened, kMuted, bs))
            client_->setDeafened(!s.deafened);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("Stop hearing others (also mutes your mic)"));
        ImGui::SameLine();
        if (toggleButton(s.noiseSuppression ? T("Noise removal: On") : T("Noise removal: Off"), s.noiseSuppression, kAccent, bs))
            client_->setNoiseSuppression(!s.noiseSuppression);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(T("Noise suppression: removes the fan, the keyboard, the street... from your mic\nOthers hear you "
                                "more clearly. It is remembered for next time."));
        ImGui::SameLine();
        std::string share = s.sharing ? T("Stop sharing (") + std::to_string(s.viewers) + T(" watching)") : T("Share screen");
        if (toggleButton(share.c_str(), s.sharing, kAccent, bs)) {
            if (s.sharing) client_->submit("/stopshare");
            else {  // what? each screen and each open window
                sources_ = ScreenCapturer::sources();
                audioApps_ = SystemAudioCapture::apps();
                ImGui::OpenPopup("##fuente");
            }
        }
        if (ImGui::BeginPopup("##fuente")) {
            ImGui::TextDisabled(T("What do you want to share?"));
            for (auto& src : sources_)
                if (ImGui::Selectable(src.label.c_str())) {
                    client_->submit("/share " + std::to_string(cfg_.shareFps) + " " + std::to_string(cfg_.shareWidth) +
                                    " " + src.id);
                    ImGui::CloseCurrentPopup();
                }
            if (s.shareAudio) {
                ImGui::Separator();
                audioAppCombo();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (toggleButton(s.shareAudio ? T("PC audio: On") : T("PC audio: Off"), s.shareAudio, kAccent, bs)) {
            cfg_.shareAudio = !s.shareAudio;
            client_->setShareAudio(cfg_.shareAudio);
            cfg_.save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(T("When sharing the screen, also send what plays on your PC (game, video, music).\nThe channel's "
                                "voices are not included. It can be changed while you share.\nFrom which application: %s "
                                "(chosen when you press Share screen or in Settings > Screen)."),
                              cfg_.shareAudioApp.empty() ? T("the whole PC") : cfg_.shareAudioApp.c_str());
        ImGui::SameLine();
        if (ImGui::Button(T("Settings"), bs)) openSettings();

        // On the right: voice indicator and mic level
        float meterW = px(120);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + px(10), right - meterW - px(110)));
        const char* mode = s.pushToTalk ? "PTT" : T("Voice");
        ImVec4 col = s.talking ? kTalk : (s.muted ? kMuted : kDim);
        ImGui::AlignTextToFramePadding();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (bs.y - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::TextColored(col, "%s %s", s.talking ? T("Talking") : (s.muted ? T("Muted") : T("Idle")), mode);
        ImGui::SameLine(right - meterW);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFrameHeight() * 0.5f - px(4));
        levelMeter(s.muted ? 0.f : s.micLevel, s.threshold, ImVec2(meterW, px(8)));
        ImGui::Spacing();
    }

    // ---------------------------------------------------------- Channel tree
    void channelTree(const UiState& s) {
        ImGui::TextDisabled("%s", T("CHANNELS"));
        ImGui::Spacing();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float lineH = ImGui::GetTextLineHeight() + px(8);
        float fontH = ImGui::GetTextLineHeight();
        for (auto& ch : s.channels) {
            int count = 0;
            for (auto& u : s.users) count += u.channel == ch;
            bool current = ch == s.channel;
            ImGui::PushID(ch.c_str());
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Selectable("##ch", current, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, lineH));
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !current) client_->submit("/join " + ch);
            if (ImGui::IsItemHovered() && !current) ImGui::SetTooltip(T("Double-click to join"));
            if (ImGui::BeginPopupContextItem("chmenu")) {
                if (!current && ImGui::MenuItem(T("Join"))) client_->submit("/join " + ch);
                if (isAdmin_ && ImGui::MenuItem(T("Delete channel"))) client_->submit("/deletechannel " + ch);
                if (isMod_ && ImGui::MenuItem(T("Create channel..."))) openCreateChannel_ = true;
                ImGui::EndPopup();
            }
            float y = p.y + (lineH - fontH) * 0.5f;
            float x = p.x + px(4);
            // Icono: altavoz sencillo
            ImU32 icon = ImGui::GetColorU32(current ? kAccent : kDim);
            float cy = p.y + lineH * 0.5f;
            dl->AddRectFilled(ImVec2(x, cy - px(3)), ImVec2(x + px(4), cy + px(3)), icon, px(1));
            dl->AddTriangleFilled(ImVec2(x + px(3), cy - px(3)), ImVec2(x + px(9), cy - px(7)), ImVec2(x + px(9), cy + px(7)), icon);
            dl->AddTriangleFilled(ImVec2(x + px(3), cy - px(3)), ImVec2(x + px(9), cy + px(7)), ImVec2(x + px(3), cy + px(3)), icon);
            x += px(16);
            dl->AddText(ImVec2(x, y), ImGui::GetColorU32(current ? kText : ImVec4(0.78f, 0.80f, 0.83f, 1.f)), ch.c_str());
            if (count) {
                std::string n = std::to_string(count);
                float tw = ImGui::CalcTextSize(n.c_str()).x;
                dl->AddText(ImVec2(p.x + ImGui::GetContentRegionAvail().x - tw - px(6), y), ImGui::GetColorU32(kDim), n.c_str());
            }
            for (auto& u : s.users)
                if (u.channel == ch) userRow(s, u, lineH, fontH);
            ImGui::PopID();
        }
        if (isMod_) {
            ImGui::Spacing();
            if (ImGui::SmallButton(T("+ Create channel"))) openCreateChannel_ = true;
        }
    }

    void userRow(const UiState& s, const UiUser& u, float lineH, float fontH) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushID(int(u.id));
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool watchingThis = s.watching == u.id;
        ImGui::Selectable("##u", watchingThis, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, lineH));
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseDoubleClicked(0) && !u.me) {
                if (u.sharing) client_->submit(watchingThis ? "/unwatch" : "/watch " + u.nick);
                else prefill("/pm " + u.nick + " ");
            }
            std::string tip = u.nick + (u.registered ? T(" (registered account)") : T(" (guest)"));
            if (u.role == Role::Admin) tip += "\nAdmin";
            else if (u.role == Role::Mod) tip += "\nModerador";
            if (u.sharing) tip += u.screenAudio ? T("\nSharing their screen with their PC audio: double-click to watch it")
                                                : T("\nSharing their screen: double-click to watch it");
            else if (!u.me) tip += T("\nDouble-click: private message");
            if (u.muted) tip += T("\nMuted by a moderator");
            if (u.blocked) tip += T("\nTheir identity changed since you verified it! Right-click > Check");
            else if (u.verified) tip += T("\nIdentity verified");
            else if (!u.me) tip += T("\nIdentity not verified (right-click > Verify identity)");
            ImGui::SetTooltip("%s", tip.c_str());
        }
        if (ImGui::BeginPopupContextItem("umenu")) {
            ImGui::TextDisabled("%s", u.nick.c_str());
            ImGui::Separator();
            if (!u.me) {
                // Volume of this person (only for you; remembered by nick)
                int vol = int(u.volume * 100 + 0.5f);
                ImGui::SetNextItemWidth(px(200));
                if (ImGui::SliderInt("##uvol", &vol, 0, 300, T("Volume %d %%"))) client_->setUserVolume(u.nick, vol / 100.f);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("It only changes how you hear them. Ctrl+click to type the number."));
                if (vol != 100) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("100 %")) client_->setUserVolume(u.nick, 1.f);
                }
                ImGui::Separator();
            }
            if (!u.me && ImGui::MenuItem(T("Private message"))) prefill("/pm " + u.nick + " ");
            if (u.sharing && !u.me) {
                if (watchingThis ? ImGui::MenuItem(T("Close their screen")) : ImGui::MenuItem(T("Watch their screen")))
                    client_->submit(watchingThis ? "/unwatch" : "/watch " + u.nick);
            }
            if (!u.me && ImGui::MenuItem(u.blocked ? T("Their identity changed! Check...") : T("Verify identity..."))) {
                verifyNick_ = u.nick;
                openVerify_ = true;
            }
            if (isMod_ && !u.me) {
                ImGui::Separator();
                if (ImGui::BeginMenu(T("Move to"))) {
                    for (auto& ch : s.channels)
                        if (ch != u.channel && ImGui::MenuItem(ch.c_str())) client_->submit("/move " + u.nick + " " + ch);
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem(u.muted ? T("Unmute") : T("Mute")))
                    client_->submit((u.muted ? "/unsilence " : "/silence ") + u.nick);
                if (ImGui::MenuItem(T("Kick"))) client_->submit("/kick " + u.nick);
            }
            if (isAdmin_ && !u.me) {
                if (ImGui::MenuItem(T("Ban"))) client_->submit("/ban " + u.nick);
                if (u.registered && ImGui::BeginMenu(T("Role"))) {
                    if (ImGui::MenuItem(T("Admin"), nullptr, u.role == Role::Admin)) client_->submit("/role " + u.nick + " admin");
                    if (ImGui::MenuItem(T("Moderator"), nullptr, u.role == Role::Mod)) client_->submit("/role " + u.nick + " mod");
                    if (ImGui::MenuItem(T("User"), nullptr, u.role == Role::User)) client_->submit("/role " + u.nick + " user");
                    ImGui::EndMenu();
                }
            }
            ImGui::EndPopup();
        }
        float x = p.x + px(26), cy = p.y + lineH * 0.5f, y = p.y + (lineH - fontH) * 0.5f;
        bool silent = u.muted || (u.me && s.muted);
        ImVec4 dot = u.talking ? kTalk : (silent ? kMuted : ImVec4(0.32f, 0.35f, 0.39f, 1.f));
        if (u.talking) dl->AddCircle(ImVec2(x, cy), px(7.5f), ImGui::GetColorU32(ImVec4(kTalk.x, kTalk.y, kTalk.z, 0.45f)), 0, px(2));
        dl->AddCircleFilled(ImVec2(x, cy), px(5), ImGui::GetColorU32(dot));
        x += px(12);
        if (u.role != Role::User) {
            const char* tag = u.role == Role::Admin ? "@" : "+";
            dl->AddText(ImVec2(x, y), ImGui::GetColorU32(u.role == Role::Admin ? kAdmin : kMod), tag);
            x += ImGui::CalcTextSize(tag).x + px(2);
        }
        dl->AddText(ImVec2(x, y), ImGui::GetColorU32(u.me ? kText : nickColor(u.nick)), u.nick.c_str());
        x += ImGui::CalcTextSize(u.nick.c_str()).x + px(6);
        auto chip = [&](const char* t, const ImVec4& c) {
            ImVec2 ts = ImGui::CalcTextSize(t);
            float sc = 0.85f;
            ImVec2 sz(ts.x * sc + px(8), fontH * sc + px(2));
            float top = cy - sz.y * 0.5f;
            dl->AddRectFilled(ImVec2(x, top), ImVec2(x + sz.x, top + sz.y), ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.22f)), px(4));
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * sc, ImVec2(x + px(4), top + px(1)), ImGui::GetColorU32(c), t);
            x += sz.x + px(4);
        };
        if (u.me) chip(T("you"), kDim);
        if (u.blocked) chip(T("identity changed!"), kWarn);
        else if (u.verified) chip(T("verified"), kTalk);
        if (u.sharing) chip(watchingThis ? T("watching") : (u.screenAudio ? T("screen + audio") : T("screen")), kMod);
        if (u.muted) chip(T("muted"), kMuted);
        if (!u.me && std::fabs(u.volume - 1.f) > 0.01f) {
            std::string v = std::to_string(int(u.volume * 100 + 0.5f)) + "%";
            chip(v.c_str(), u.volume > 1.f ? kAdmin : kDim);
        }
        ImGui::PopID();
    }

    // ---------------------------------------------------------- Chat
    void chatPanel(const UiState& s) {
        float inputH = ImGui::GetFrameHeightWithSpacing() + px(4);
        ImGui::BeginChild("log", ImVec2(0, -inputH), ImGuiChildFlags_AlwaysUseWindowPadding);
        {
            std::lock_guard<std::mutex> l(logMu);
            bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - px(4);
            ImGui::PushTextWrapPos(0);
            // Only the visible lines are drawn (with a margin); the rest take their already measured height. That way a long
            // chat (up to 3000 lines) costs the same as a short one on every repaint.
            float width = ImGui::GetContentRegionAvail().x;
            float top = ImGui::GetScrollY() - px(200), bottom = ImGui::GetScrollY() + ImGui::GetWindowHeight() + px(200);
            float gap = 0, spacing = ImGui::GetStyle().ItemSpacing.y;
            for (auto& line : logLines) {
                float y = ImGui::GetCursorPosY() + gap;
                if (line.heightAt == width && (y + line.height < top || y > bottom)) {
                    gap += line.height;
                    continue;
                }
                if (gap > 0) {
                    ImGui::Dummy(ImVec2(0, std::max(0.f, gap - spacing)));
                    gap = 0;
                }
                float y0 = ImGui::GetCursorPosY();
                logLine(line);
                line.height = ImGui::GetCursorPosY() - y0;
                line.heightAt = width;
            }
            if (gap > 0) ImGui::Dummy(ImVec2(0, std::max(0.f, gap - spacing)));
            ImGui::PopTextWrapPos();
            if (logDirty && atBottom) ImGui::SetScrollHereY(1.f);
            logDirty = false;
        }
        if (ImGui::BeginPopupContextWindow("logmenu")) {
            if (ImGui::MenuItem(T("Copy the whole chat"))) {
                std::string all;
                std::lock_guard<std::mutex> l(logMu);
                for (auto& line : logLines) all += line.time + "  " + line.text + "\n";
                ImGui::SetClipboardText(all.c_str());
            }
            if (ImGui::MenuItem(T("Clear"))) {
                std::lock_guard<std::mutex> l(logMu);
                logLines.clear();
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();

        std::string hint = s.joined ? T("Message to ") + s.channel + T("  (or /command, /help)") : T("Joining...");
        ImGui::SetNextItemWidth(-px(90));
        if (focusInput_) {
            ImGui::SetKeyboardFocusHere();
            focusInput_ = false;
        }
        curState_ = &s;
        ImVec2 inPos = ImGui::GetCursorScreenPos();
        // After filling the field (e.g. "/pm nick ") the cursor goes to the end; Tab completes commands, nicks and channels
        auto cb = [](ImGuiInputTextCallbackData* d) {
            App* app = static_cast<App*>(d->UserData);
            if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
                std::string now(d->Buf, size_t(d->BufTextLen));
                std::string next = app->complete(now);
                if (next != now) {
                    d->DeleteChars(0, d->BufTextLen);
                    d->InsertChars(0, next.c_str());
                }
            } else if (app->moveCursorEnd_) {
                d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen;
                app->moveCursorEnd_ = false;
            }
            return 0;
        };
        bool sent = ImGui::InputTextWithHint("##in", hint.c_str(), &input_,
                                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways |
                                                 ImGuiInputTextFlags_CallbackCompletion,
                                             cb, this);
        bool typing = ImGui::IsItemActive();
        ImGui::SameLine();
        sent |= ImGui::Button(T("Send"), ImVec2(-1, 0));
        if (sent) {
            if (!input_.empty()) {
                if (input_ == "/quit" || input_ == "/salir") requestQuit();
                else client_->submit(input_);
                input_.clear();
            }
            focusInput_ = true;
            compLast_.clear();
        }
        if (typing) suggestions(inPos);
        curState_ = nullptr;
    }

    // ---------------------------------------------------------- Chat autocompletion
    static std::string lowerAscii(std::string t) {
        for (auto& ch : t) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return t;
    }

    struct Completion {
        std::vector<std::string> items, notes;
        size_t start = 0;  // where what is replaced starts
    };

    // What can be completed in what is typed (with the cursor at the end): a command, or the nick or channel being typed
    Completion completions(const std::string& text) const {
        Completion c;
        if (!curState_) return c;
        const UiState& s = *curState_;
        size_t firstSp = text.find(' ');
        if (!text.empty() && text[0] == '/' && firstSp == std::string::npos) {  // the command itself
            std::string w = lowerAscii(text.substr(1));
            for (auto& k : kCommands) {
                if (s.role < k.role || std::string(shownName(k)).rfind(w, 0) != 0) continue;
                c.items.push_back(std::string("/") + shownName(k));
                c.notes.push_back(std::string(T(k.args)) + (k.args[0] ? "   " : "") + T(k.desc));
            }
            return c;
        }
        ArgKind kind = ArgKind::Nick;  // in a normal message, the word being typed is completed with nicks
        size_t lastSp = text.find_last_of(' ');
        c.start = lastSp == std::string::npos ? 0 : lastSp + 1;
        if (!text.empty() && text[0] == '/') {
            kind = ArgKind::None;
            std::string cmd = lowerAscii(text.substr(1, firstSp - 1));
            const CommandInfo* info = nullptr;
            for (auto& k : kCommands)
                if (cmd == k.name || cmd == k.es) info = &k;
            if (info && info->first == ArgKind::Channel) {  // channels can contain spaces: everything that follows
                kind = ArgKind::Channel;
                c.start = firstSp + 1;
            } else if (info && info->first == ArgKind::Nick && c.start == firstSp + 1) {
                kind = ArgKind::Nick;  // only the first argument
            }
        }
        std::string w = lowerAscii(text.substr(c.start));
        if (kind == ArgKind::Nick && w.empty() && (text.empty() || text[0] != '/')) return c;
        if (kind == ArgKind::Nick)
            for (auto& u : s.users)
                if (!u.me && lowerAscii(u.nick).rfind(w, 0) == 0) {
                    c.items.push_back(u.nick);
                    c.notes.push_back(u.channel == s.channel ? T("in your channel") : "en " + u.channel);
                }
        if (kind == ArgKind::Channel)
            for (auto& ch : s.channels)
                if (lowerAscii(ch).rfind(w, 0) == 0) {
                    c.items.push_back(ch);
                    c.notes.push_back(ch == s.channel ? T("you are here") : T("channel"));
                }
        return c;
    }

    // Tab: completes; if pressed again, moves to the next match
    std::string complete(const std::string& text) {
        if (!compLast_.empty() && text == compLast_ && compList_.items.size() > 1) {
            compIdx_ = (compIdx_ + 1) % compList_.items.size();
        } else {
            compList_ = completions(text);
            if (compList_.items.empty()) return text;
            compBase_ = text;
            compIdx_ = 0;
        }
        compLast_ = compBase_.substr(0, compList_.start) + compList_.items[compIdx_] + " ";
        return compLast_;
    }

    // List of commands (or of nicks and channels for their first argument) above the text box while typing
    void suggestions(ImVec2 at) {
        if (input_.empty() || input_[0] != '/') return;  // in normal messages, nicks are completed with Tab
        bool cycling = input_ == compLast_;
        Completion c = cycling ? compList_ : completions(input_);
        if (c.items.empty()) return;
        // If it is already fully typed and there are no other options, the list isn't needed... except to see what it takes
        ImGui::SetNextWindowPos(ImVec2(at.x, at.y - px(4)), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowBgAlpha(0.97f);
        ImGui::Begin("##sugerencias", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoMove);
        size_t n = std::min<size_t>(c.items.size(), 8);
        size_t first = cycling && compIdx_ >= n ? compIdx_ - n + 1 : 0;  // keep the chosen one visible
        for (size_t i = first; i < first + n && i < c.items.size(); ++i) {
            bool sel = cycling && i == compIdx_;
            ImGui::TextColored(sel ? kTalk : kAccent, "%s", c.items[i].c_str());
            ImGui::SameLine(px(150));
            ImGui::TextDisabled("%s", c.notes[i].c_str());
        }
        if (c.items.size() > n) ImGui::TextDisabled(T("... and %d more"), int(c.items.size() - n));
        ImGui::TextDisabled(T("Tab to complete%s"), c.items.size() > 1 ? T(" (again: the next one)") : "");
        ImGui::End();
    }

    // ---------------------------------------------------------- First-time assistant
    void startWizAudio() {
        if (wizAudio_) return;
        wizAudio_ = std::make_unique<Audio>();
        wizAudio_->loadChoice();
        wizAudio_->vadThreshold = cfg_.threshold;
        wizAudio_->start([](const uint8_t*, size_t) {});  // only to measure: nothing is sent
        wizIns_.clear();
        wizOuts_.clear();
        for (auto& d : wizAudio_->inputs()) wizIns_.push_back(d.name);
        for (auto& d : wizAudio_->outputs()) wizOuts_.push_back(d.name);
    }
    void stopWizAudio() {
        if (!wizAudio_) return;
        wizAudio_->saveChoice();  // the choice is kept for when you connect
        wizAudio_->shutdown();
        wizAudio_.reset();
    }
    void finishWizard() {
        stopWizAudio();
        cfg_.wizardDone = true;
        cfg_.save();
        ImGui::CloseCurrentPopup();
    }

    void deviceCombo(const char* id, bool input) {
        std::string& choice = input ? wizAudio_->inputChoice : wizAudio_->outputChoice;
        std::string cur = choice.empty() ? std::string(T("Automatic (")) + (input ? wizAudio_->inputName() : wizAudio_->outputName()) + ")"
                                         : choice;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo(id, cur.c_str())) {
            auto pick = [&](const std::string& v) {
                choice = v;
                wizAudio_->restart();
            };
            if (ImGui::Selectable(T("Automatic (the one Windows uses)"), choice.empty())) pick("");
            for (auto& n : input ? wizIns_ : wizOuts_)
                if (ImGui::Selectable(n.c_str(), n == choice)) pick(n);
            ImGui::EndCombo();
        }
    }

    void wizardModal() {
        const char* kTitle = T("Welcome to Eco");
        if (openWizard_) {
            ImGui::OpenPopup(kTitle);
            openWizard_ = false;
            wizStep_ = 0;
        }
        ImGui::SetNextWindowSize(ImVec2(px(540), 0), ImGuiCond_Always);
        if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            stopWizAudio();
            return;
        }
        const int kSteps = 5;
        if (wizStep_ != wizShown_) { wizShown_ = wizStep_; wizFocus_ = true; }  // new step: focus on the field
        ImGui::TextDisabled(T("Step %d of %d"), wizStep_ + 1, kSteps);
        ImGui::PushTextWrapPos(0);
        bool canNext = true;
        if (wizStep_ != 2) stopWizAudio();
        switch (wizStep_) {
            case 0:
                ImGui::TextColored(kAccent, T("Voice and chat with your friends, end-to-end encrypted"));
                ImGui::Spacing();
                ImGui::BulletText(T("Voice channels where you talk without pressing anything (or with a key, if you prefer)."));
                ImGui::BulletText(T("Chat, private messages and screen sharing (with its sound, if you want)."));
                ImGui::BulletText(T("Not even the server can read what you write or hear what you say."));
                ImGui::Spacing();
                ImGui::TextUnformatted(T("In a minute we'll have it ready: your name, your microphone and the server to join."));
                ImGui::Spacing();
                languageCombo();
                break;
            case 1:
                ImGui::TextColored(kAccent, T("What's your name?"));
                ImGui::TextUnformatted(T("It's the name others will see you by."));
                ImGui::SetNextItemWidth(-1);
                if (wizFocus_) { ImGui::SetKeyboardFocusHere(); wizFocus_ = false; }
                ImGui::InputTextWithHint("##wiznick", T("For example: John"), &nick_);
                if (nick_.find(' ') != std::string::npos) {
                    ImGui::TextColored(kWarn, T("No spaces: for example John_S"));
                    canNext = false;
                } else if (nick_.empty() || nick_.size() > 24) {
                    canNext = false;
                }
                ImGui::TextDisabled(T("Later you can reserve it so nobody else uses it: Account menu > Reserve my name."));
                break;
            case 2: {
                startWizAudio();
                ImGui::TextColored(kAccent, T("Your microphone and your headphones"));
                ImGui::TextUnformatted(T("Microphone:"));
                deviceCombo("##wizmic", true);
                ImGui::TextUnformatted(T("Where you hear the others:"));
                deviceCombo("##wizout", false);
                ImGui::Spacing();
                ImGui::TextUnformatted(T("Say something: the bar should move and cross the line when you speak."));
                float lvl = std::min(1.f, wizAudio_->micLevel.load() / 0.1f);
                float thr = std::min(1.f, cfg_.threshold / 0.1f);
                ImVec2 p = ImGui::GetCursorScreenPos();
                float w = ImGui::GetContentRegionAvail().x, h = px(18);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), px(3));
                dl->AddRectFilled(p, ImVec2(p.x + w * lvl, p.y + h), ImGui::GetColorU32(lvl >= thr ? kTalk : kDim), px(3));
                dl->AddLine(ImVec2(p.x + w * thr, p.y - px(2)), ImVec2(p.x + w * thr, p.y + h + px(2)), IM_COL32(255, 255, 255, 220), px(2));
                ImGui::Dummy(ImVec2(w, h));
                int sens = int(cfg_.threshold * 1000 + 0.5f);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("##wizthr", &sens, 1, 60, T("You need to speak this loud: %d"))) {
                    cfg_.threshold = sens / 1000.f;
                    wizAudio_->vadThreshold = cfg_.threshold;
                }
                ImGui::TextDisabled(T("If the bar doesn't move, choose another microphone. If it crosses the line without you "
                                      "speaking, move the line to the right. With headphones, the others don't hear themselves."));
                break;
            }
            case 3:
                ImGui::TextColored(kAccent, T("Which server are you joining?"));
                ImGui::TextUnformatted(T("Paste the invitation you were given (it starts with eco://). Anyone already inside can get it "
                                         "from the Eco menu > Invite a friend."));
                ImGui::SetNextItemWidth(-px(80));
                ImGui::InputTextWithHint("##wizsrv", T("eco://...   or   192.168.1.10"), &server_);
                ImGui::SameLine();
                if (ImGui::Button(T("Paste"), ImVec2(-1, 0)))
                    if (const char* clip = ImGui::GetClipboardText()) server_ = clip;
                canNext = !server_.empty();
                ImGui::Spacing();
                ImGui::TextDisabled(T("Running the server yourself? Open eco-server on the PC that will be the server: when it starts "
                                      "it shows the invitation for your friends."));
                break;
            case 4:
                ImGui::TextColored(kAccent, T("All set!"));
                ImGui::BulletText(T("Name: %s"), nick_.c_str());
                ImGui::BulletText(T("Server: %s"), isInvite(server_) ? T("eco:// invitation") : server_.c_str());
                ImGui::Spacing();
                ImGui::TextUnformatted(T("Once inside: double-click a channel to switch to it, double-click someone who is sharing to "
                                         "watch their screen, and in the chat type / to see the commands (Tab completes commands and "
                                         "names)."));
                break;
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
        if (wizStep_ > 0 && ImGui::Button(T("Back"))) --wizStep_;
        if (wizStep_ > 0) ImGui::SameLine();
        if (wizStep_ < kSteps - 1) {
            ImGui::BeginDisabled(!canNext);
            if (ImGui::Button(wizStep_ == 0 ? T("Start") : T("Next"))) ++wizStep_;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(T("Skip the assistant"))) finishWizard();
        } else {
            if (ImGui::Button(T("Connect"))) {
                finishWizard();
                startConnect();
            }
            ImGui::SameLine();
            if (ImGui::Button(T("Close"))) finishWizard();
        }
        ImGui::EndPopup();
    }

    void logLine(const LogLine& l) {
        ImGui::TextColored(ImVec4(0.42f, 0.45f, 0.50f, 1.f), "%s", l.time.c_str());
        ImGui::SameLine();
        switch (l.kind) {
            case Kind::Chat:
            case Kind::Private: {
                bool priv = l.kind == Kind::Private;
                std::string who = l.text.substr(0, l.nickLen);
                std::string nick = who;
                if (!priv && !nick.empty()) nick.pop_back();  // quitar ':'
                ImGui::TextColored(priv ? kPriv : nickColor(nick), "%s", who.c_str());
                ImGui::SameLine(0, px(5));
                std::string rest = l.text.size() > l.nickLen + 1 ? l.text.substr(l.nickLen + 1) : "";
                if (priv) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.84f, 0.98f, 1.f));
                ImGui::TextUnformatted(rest.c_str());
                if (priv) ImGui::PopStyleColor();
                break;
            }
            case Kind::Warn: ImGui::TextColored(kWarn, "%s", l.text.c_str() + 2); break;
            case Kind::Info: ImGui::TextColored(kDim, "%s", l.text.rfind("* ", 0) == 0 ? l.text.c_str() + 2 : l.text.c_str()); break;
            case Kind::Header: ImGui::TextColored(kAccent, "%s", l.text.c_str()); break;
            case Kind::Block: {  // lists and history: all lines aligned after the time
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.78f, 0.82f, 1.f));
                ImGui::TextUnformatted(l.text.c_str() + (l.text[0] == '\n' ? 1 : 0));
                ImGui::PopStyleColor();
                break;
            }
        }
    }

    void statusBar(const UiState& s) {
        ImGui::AlignTextToFramePadding();
        if (s.voiceTcp) {
            ImGui::TextColored(kWarn, T("Voice over TCP"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(T("Your network doesn't let UDP through (firewall or corporate network?): voice goes over TCP, "
                                    "with a bit more\ndelay. If the server is yours, open port 9987 UDP. When UDP works, Eco "
                                    "switches back by itself."));
        } else {
            ImVec4 dot = s.udpOk ? kTalk : kWarn;
            ImGui::TextColored(dot, "%s", s.udpOk ? T("Connected") : T("Connecting voice..."));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(s.udpOk ? T("Voice (UDP) is arriving fine") : T("No UDP response yet: if it stays like this, check the firewall (port 9987 UDP)"));
        }
        if (s.pingMs >= 0) {
            ImGui::SameLine();
            ImVec4 pc = s.pingMs < 80 ? kDim : (s.pingMs < 200 ? kAdmin : kWarn);
            ImGui::TextColored(pc, T("· %d ms"), s.pingMs);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("Ping: how long a packet takes to reach the server and come back"));
        }
        if (s.lossPct >= 0) {
            ImGui::SameLine();
            ImVec4 lc = s.lossPct < 1 ? kDim : (s.lossPct < 5 ? kAdmin : kWarn);
            ImGui::TextColored(lc, T("· loss %.1f %%"), double(s.lossPct));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(T("Voice that didn't arrive in the last few seconds. Above 5 %% it is noticeable: it's usually "
                                    "the wifi or the connection."));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("· %s · %s (%s%s)", hostLabel_.c_str(), s.nick.c_str(), T(roleLabel(s.role)),
                            s.registered ? T(", registered") : T(", guest"));
        if (s.serverVerified) {
            ImGui::SameLine();
            ImGui::TextColored(kTalk, T("· server verified"));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("The server's identity matches the one in the invitation"));
        }
        ImGui::SameLine();
        if (s.e2ePhrase) ImGui::TextColored(kAccent, T("· E2E encryption with phrase (%s)"), s.e2eCheck.c_str());
        else ImGui::TextColored(kAccent, T("· End-to-end encryption"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(T("Not even the server can read the chat or hear the voice.%s\nRight-click a person > Verify "
                                "identity, to check that nobody is in the middle."),
                              s.serverVerified ? T("\nThe server's identity was checked against the invitation.") : "");
        if (s.watching) {
            ImGui::SameLine();
            ImGui::TextColored(kMod, T("· Watching a screen"));
        }
    }

    // ---------------------------------------------------------- Ventanas emergentes
    void passwordModal(const UiState& s) {
        if (!s.authPrompt.empty() && s.authSeq > authHandled_ && !ImGui::IsPopupOpen(T("Password"))) {
            authMsg_ = s.authPrompt;
            authPass_.clear();
            ImGui::OpenPopup(T("Password"));
        }
        ImGui::SetNextWindowSize(ImVec2(px(400), 0));
        if (ImGui::BeginPopupModal(T("Password"), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(authMsg_.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-1);
            bool ok = ImGui::InputText("##ap", &authPass_, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
            ok |= ImGui::Button(T("Join"), ImVec2(px(120), 0));
            ImGui::SameLine();
            if (ImGui::Button(T("Cancel"), ImVec2(px(120), 0))) {
                ImGui::CloseCurrentPopup();
                authHandled_ = s.authSeq;
                loginError_ = T("Login cancelled");
                ImGui::EndPopup();
                disconnect();
                return;
            }
            if (ok && !authPass_.empty()) {
                accountPass_ = authPass_;
                client_->submit(authPass_);
                authPass_.clear();
                authHandled_ = s.authSeq;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void createChannelModal() {
        if (openCreateChannel_) {
            ImGui::OpenPopup(T("Create channel"));
            newChannel_.clear();
            openCreateChannel_ = false;
        }
        ImGui::SetNextWindowSize(ImVec2(px(360), 0));
        if (ImGui::BeginPopupModal(T("Create channel"), nullptr, ImGuiWindowFlags_NoResize)) {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-1);
            bool ok = ImGui::InputTextWithHint("##nc", T("Channel name"), &newChannel_, ImGuiInputTextFlags_EnterReturnsTrue);
            ok |= ImGui::Button(T("Create"), ImVec2(px(110), 0));
            ImGui::SameLine();
            if (ImGui::Button(T("Cancel"), ImVec2(px(110), 0))) ImGui::CloseCurrentPopup();
            if (ok && !newChannel_.empty()) {
                client_->submit("/createchannel " + newChannel_);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // Verifying someone in plain language: you both read the numbers by phone or in person
    void verifyModal(const UiState& s) {
        if (openVerify_) {
            ImGui::OpenPopup(T("Verify identity"));
            openVerify_ = false;
        }
        ImGui::SetNextWindowSize(ImVec2(px(480), 0));
        if (!ImGui::BeginPopupModal(T("Verify identity"), nullptr, ImGuiWindowFlags_NoResize)) return;
        const UiUser* u = nullptr;
        for (auto& x : s.users)
            if (x.nick == verifyNick_) u = &x;
        ImGui::PushTextWrapPos(0);
        if (!u) {
            ImGui::TextUnformatted((verifyNick_ + T(" is no longer connected.")).c_str());
        } else {
            ImGui::Text(T("Safety code between you and %s:"), u->nick.c_str());
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, u->blocked ? kWarn : kAdmin);
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
            ImGui::TextUnformatted(u->safetyCode.c_str());
            ImGui::PopFont();
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (u->blocked) {
                ImGui::TextColored(kWarn, T("Warning! This code is no longer the one you verified."));
                ImGui::TextUnformatted(
                    T("They may have changed PC or reinstalled Eco, but it could also be someone pretending to be "
                      "them. Meanwhile, Eco doesn't let them read what you write or show you their messages. Call "
                      "them or ask them in person and compare these numbers."));
                ImGui::Spacing();
                if (ImGui::Button(T("We checked it and they match"), ImVec2(-1, px(32)))) {
                    client_->submit("/accept " + u->nick);
                    ImGui::CloseCurrentPopup();
                }
            } else if (u->verified) {
                ImGui::TextColored(kTalk, T("Verified: it's the same person you checked."));
                ImGui::TextDisabled(T("If it ever changes, Eco will warn you and block it until you check it."));
            } else {
                ImGui::TextUnformatted(
                    T("To be sure nobody is in the middle (not even the server), compare this number: by phone, in "
                      "person or through another app. They get the same one if everything is fine."));
                ImGui::Spacing();
                if (ImGui::Button(T("They match: mark as verified"), ImVec2(-1, px(32)))) {
                    client_->submit("/confirm " + u->nick);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::TextDisabled(T("If they DON'T match, don't tell them anything private and warn the server owner."));
            }
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(T("Close"), ImVec2(px(120), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void inviteModal(const UiState& s) {
        if (openInvite_) {
            ImGui::OpenPopup(T("Invite a friend"));
            openInvite_ = false;
        }
        ImGui::SetNextWindowSize(ImVec2(px(520), 0));
        if (!ImGui::BeginPopupModal(T("Invite a friend"), nullptr, ImGuiWindowFlags_NoResize)) return;
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(
            T("Send them this invitation (by WhatsApp, email...). They have to open Eco, paste it into "
              "\"Server or invitation\", type their name and press Connect. Eco will check by itself that the "
              "server is the real one."));
        ImGui::Spacing();
        std::string inv = s.invite;
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##inv", &inv, ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button(T("Copy invitation"), ImVec2(px(180), px(30)))) ImGui::SetClipboardText(s.invite.c_str());
        ImGui::Spacing();
        if (s.inviteHasPass) ImGui::TextColored(kWarn, T("It contains the server's password: only give it to people you trust."));
        if (hostLabel_.find("127.0.0.1") != std::string::npos || hostLabel_.find("localhost") != std::string::npos)
            ImGui::TextColored(kWarn, T("You are connected with 127.0.0.1, which only works on this PC: in the invitation, change that "
                                        "part to your IP or domain."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(T("Close"), ImVec2(px(120), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Create account, change password or raise the role: the text is hidden and repeated to avoid mistakes
    void accountPassModal() {
        const char* kTitle = T("Account password###acctpass");
        if (openPass_ != PassMode::None) {
            passMode_ = openPass_;
            openPass_ = PassMode::None;
            pass1_.clear();
            pass2_.clear();
            passError_.clear();
            ImGui::OpenPopup(kTitle);
        }
        ImGui::SetNextWindowSize(ImVec2(px(440), 0));
        if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoResize)) return;
        bool login = passMode_ == PassMode::Login;
        ImGui::PushTextWrapPos(0);
        if (passMode_ == PassMode::Register)
            ImGui::TextUnformatted(T("Reserve your name: nobody else will be able to use it. Next time Eco will ask you for this password."));
        else if (passMode_ == PassMode::Change)
            ImGui::TextUnformatted(T("Type your new password."));
        else
            ImGui::TextUnformatted(T("Moderator or administrator password the server owner gave you."));
        ImGui::PopTextWrapPos();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        bool ok = ImGui::InputTextWithHint("##p1", login ? T("Mod/admin password") : T("Password (at least 8 characters)"), &pass1_,
                                           ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        if (!login) {
            ImGui::SetNextItemWidth(-1);
            ok |= ImGui::InputTextWithHint("##p2", T("Repeat it"), &pass2_,
                                           ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        }
        if (!passError_.empty()) ImGui::TextColored(kWarn, "%s", passError_.c_str());
        ok |= ImGui::Button(T("OK"), ImVec2(px(120), 0));
        ImGui::SameLine();
        bool cancel = ImGui::Button(T("Cancel"), ImVec2(px(120), 0));
        if (ok && !cancel) {
            if (pass1_.empty()) passError_ = T("Type the password");
            else if (!login && (pass1_.size() < 8 || pass1_.size() > 128)) passError_ = T("It must be between 8 and 128 characters");
            else if (!login && pass1_ != pass2_) passError_ = T("The two don't match");
            else if (pass1_.find(' ') != std::string::npos && login) passError_ = T("The password can't contain spaces");
            else {
                const char* cmd = passMode_ == PassMode::Register ? "/register " : passMode_ == PassMode::Change ? "/password " : "/login ";
                client_->submit(cmd + pass1_);
                cancel = true;
            }
        }
        if (cancel) {
            std::fill(pass1_.begin(), pass1_.end(), '\0');
            std::fill(pass2_.begin(), pass2_.end(), '\0');
            pass1_.clear();
            pass2_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void guideModal() {
        if (openGuide_) {
            ImGui::OpenPopup(T("Quick guide"));
            openGuide_ = false;
        }
        ImGui::SetNextWindowSize(ImVec2(px(540), 0));
        if (!ImGui::BeginPopupModal(T("Quick guide"), nullptr, ImGuiWindowFlags_NoResize)) return;
        ImGui::PushTextWrapPos(0);
        auto item = [&](const char* title, const char* body) {
            ImGui::TextColored(kAccent, "%s", title);
            ImGui::TextUnformatted(body);
            ImGui::Spacing();
        };
        item(T("Speaking"), T("You don't have to press anything: when you speak, the others hear you (the dot next to your "
                            "name turns green). \"Mic on\" at the top mutes it. In Settings you can choose microphone and "
                            "headphones."));
        item(T("Channels"), T("The channels are on the left. Double-click one to join. You only hear and read the people in "
                             "your channel."));
        item(T("Writing"), T("Type at the bottom and press Enter. For a private message, double-click a person."));
        item(T("Screen"), T("\"Share screen\" lets the people in your channel see yours. To watch someone else's, "
                              "double-click their name when it says \"screen\"."));
        item(T("Security"), T("Everything is encrypted: not even the server owner can read or hear anything. To be 100% sure "
                               "you are talking to who you think, right-click their name > Verify identity."));
        item(T("Invite"), T("Eco menu > Invite a friend: copy the invitation and send it to them."));
        item(T("Your name"), T("Account menu > Reserve my name, so nobody else can use it."));
        ImGui::PopTextWrapPos();
        if (ImGui::Button(T("Got it"), ImVec2(px(140), px(30)))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void openSettings() {
        showSettings_ = true;
        if (client_) client_->requestDevices();
    }

    bool deviceCombo(const char* label, const std::string& choice, const std::string& inUse,
                     const std::vector<std::string>& list, std::string& picked) {
        std::string preview = choice.empty() ? T("Automatic") + (inUse.empty() ? std::string() : " (" + inUse + ")") : choice;
        bool changed = false;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo(label, preview.c_str())) {
            if (ImGui::Selectable(T("Automatic (follows the Windows default)"), choice.empty())) {
                picked.clear();
                changed = true;
            }
            for (auto& d : list)
                if (ImGui::Selectable(d.c_str(), d == choice)) {
                    picked = d;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    }

    // Idioma / Language: takes effect right away and is saved for next time (also for eco-client)
    void languageCombo() {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Idioma / Language");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(px(160));
        i18n::Lang cur = i18n::lang();
        if (ImGui::BeginCombo("##idioma", i18n::name(cur))) {
            for (i18n::Lang l : {i18n::Lang::Es, i18n::Lang::En})
                if (ImGui::Selectable(i18n::name(l), l == cur) && l != cur) {
                    i18n::set(l);
                    i18n::save(l);
                }
            ImGui::EndCombo();
        }
    }

    void settingsWindow(const UiState& s) {
        ImGui::SetNextWindowSize(ImVec2(px(520), px(600)), ImGuiCond_FirstUseEver);
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin(T("Settings"), &showSettings_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::End();
            return;
        }
        bool dirty = false;
        languageCombo();
        ImGui::Spacing();
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem(T("Audio"))) {
                std::string picked;
                ImGui::SeparatorText(T("Microphone"));
                if (deviceCombo("##mic", s.inputChoice, s.inputName, s.inputs, picked)) client_->chooseDevice(true, picked);
                ImGui::SeparatorText(T("Output (speakers / headphones)"));
                if (deviceCombo("##out", s.outputChoice, s.outputName, s.outputs, picked)) client_->chooseDevice(false, picked);
                if (ImGui::SmallButton(T("Search for devices again"))) client_->submit("/devices");

                ImGui::SeparatorText(T("Volume of the others"));
                int vol = int(s.volume * 100 + 0.5f);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("##vol", &vol, 0, 200, "%d %%")) {
                    client_->setVolume(vol / 100.f);
                    cfg_.volume = vol / 100.f;
                    dirty = true;
                }

                ImGui::SeparatorText(T("How your microphone turns on"));
                bool ptt = !cfg_.ptt.empty();
                if (ImGui::RadioButton(T("Voice detection"), !ptt) && ptt) {
                    cfg_.ptt.clear();
                    client_->setPushToTalk("");
                    dirty = true;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton(T("Push to talk"), ptt) && !ptt) {
                    cfg_.ptt = "v";
                    client_->setPushToTalk(cfg_.ptt);
                    dirty = true;
                }
                if (!cfg_.ptt.empty()) {
                    static const char* keys[] = {"v", "b", "t", "x", "z", "f8", "f9", "f10", "ctrl", "alt", "shift",
                                                 "capslock", "space", "mouse4", "mouse5"};
                    ImGui::SetNextItemWidth(px(160));
                    if (ImGui::BeginCombo(T("Key (works even if Eco is not in the foreground)"), cfg_.ptt.c_str())) {
                        for (auto* k : keys)
                            if (ImGui::Selectable(k, cfg_.ptt == k)) {
                                cfg_.ptt = k;
                                client_->setPushToTalk(k);
                                dirty = true;
                            }
                        ImGui::EndCombo();
                    }
                } else {
                    ImGui::TextUnformatted(T("Sensitivity"));
                    ImGui::SameLine();
                    ImGui::TextDisabled(T("(speak: the bar must cross the orange line)"));
                    levelMeter(s.muted ? 0.f : s.micLevel, s.threshold, ImVec2(ImGui::GetContentRegionAvail().x, px(10)));
                    int th = int(s.threshold * 1000 + 0.5f);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderInt("##th", &th, 0, 100, T("threshold %d"))) {
                        client_->setThreshold(th / 1000.f);
                        cfg_.threshold = th / 1000.f;
                        dirty = true;
                    }
                }

                ImGui::SeparatorText(T("Noise suppression"));
                bool ns = s.noiseSuppression;
                if (ImGui::Checkbox(T("Remove background noise from my microphone"), &ns)) client_->setNoiseSuppression(ns);
                ImGui::TextDisabled(T("Fan, keyboard, street... It uses very little CPU and only while the mic is open."));

                ImGui::SeparatorText(T("Automatic volume of your voice"));
                bool agc = s.autoGain;
                if (ImGui::Checkbox(T("Adjust my microphone volume automatically"), &agc)) client_->setAutoGain(agc);
                if (s.autoGain) {
                    ImGui::SameLine();
                    float db = 20.f * std::log10(std::max(s.agcGain, 0.01f));
                    ImGui::TextDisabled(T("(now %+.1f dB)"), db);
                }
                ImGui::TextDisabled(T("If your mic picks up quietly it raises you, and if it clips it lowers you, so everyone hears "
                                      "you the same."));

                ImGui::SeparatorText(T("Voice quality"));
                int br = s.bitrateKbps;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("##br", &br, 8, 128, "%d kbps")) {
                    client_->setBitrate(br);
                    cfg_.bitrate = br;
                    dirty = true;
                }
                ImGui::TextDisabled(T("40 kbps sounds very good for voice; more uses more bandwidth."));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(T("Screen"))) {
                ImGui::SeparatorText(T("When sharing your screen"));
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("##fps", &cfg_.shareFps, 1, 30, "%d fotogramas por segundo")) dirty = true;
                static const int widths[] = {960, 1280, 1600, 1920, 2560, 3840};
                std::string cur = std::to_string(cfg_.shareWidth) + T(" px wide");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##w", cur.c_str())) {
                    for (int w : widths)
                        if (ImGui::Selectable((std::to_string(w) + T(" px")).c_str(), w == cfg_.shareWidth)) {
                            cfg_.shareWidth = w;
                            dirty = true;
                        }
                    ImGui::EndCombo();
                }
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled(T("Only the areas that change are sent, and only if someone is watching. It applies the next time "
                                      "you start sharing."));
                ImGui::PopTextWrapPos();
                ImGui::SeparatorText(T("PC audio while sharing"));
                if (ImGui::Checkbox(T("Also send what plays on the PC"), &cfg_.shareAudio)) {
                    client_->setShareAudio(cfg_.shareAudio);
                    dirty = true;
                }
                if (ImGui::IsItemActivated()) audioApps_ = SystemAudioCapture::apps();
                if (ImGui::Button(T("Look for applications with sound"))) audioApps_ = SystemAudioCapture::apps();
                audioAppCombo();
                ImGui::SeparatorText(T("Other people's screens"));
                int sv = int(cfg_.screenVolume * 100 + 0.5f);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("##volpant", &sv, 0, 200, T("Volume of their PC audio: %d %%"))) {
                    cfg_.screenVolume = sv / 100.f;
                    client_->setScreenVolume(cfg_.screenVolume);
                    dirty = true;
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(T("Encryption"))) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored(kAccent, T("Automatic end-to-end encryption is always on."));
                ImGui::TextUnformatted(T("To check that nobody is in the middle, right-click a user > «Verify identity» and compare the "
                                         "code some other way."));
                ImGui::SeparatorText(T("Shared phrase (optional)"));
                ImGui::TextDisabled(T("Replaces the automatic keys with ones derived from a phrase the group shares by other means. "
                                      "Everyone must use the same one."));
                ImGui::PopTextWrapPos();
                if (s.e2ePhrase) {
                    ImGui::Text(T("Enabled. Check code: "));
                    ImGui::SameLine();
                    ImGui::TextColored(kAdmin, "%s", s.e2eCheck.c_str());
                    if (ImGui::Button(T("Disable phrase"))) client_->submit("/e2e off");
                } else {
                    ImGui::SetNextItemWidth(-px(100));
                    bool go = ImGui::InputTextWithHint("##ph", T("phrase of at least 8 characters"), &phrase_,
                                                       ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::SameLine();
                    go |= ImGui::Button(T("Enable"), ImVec2(-1, 0));
                    if (go && !phrase_.empty()) {
                        client_->submit("/e2e " + phrase_);
                        phrase_.clear();
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (dirty) cfg_.save();
        ImGui::End();
    }

    void prefill(const std::string& text) {
        input_ = text;
        focusInput_ = true;
        moveCursorEnd_ = true;
    }

    float scale_;
    Settings cfg_;
    Screen screen_ = Screen::Login;
    std::unique_ptr<EcoClient> client_;
    std::future<bool> connecting_;
    std::string server_, nick_, accountPass_, serverPass_, hostLabel_;
    std::string loginError_, newHostKey_;
    bool trustNext_ = false;
    bool applied_ = false;
    std::string input_, authMsg_, authPass_, newChannel_, phrase_;
    int authHandled_ = 0;
    bool focusInput_ = false, moveCursorEnd_ = false;
    bool showSettings_ = false, openCreateChannel_ = false;
    bool isMod_ = false, isAdmin_ = false, anyTalking_ = false;
    // Help and account windows
    std::string verifyNick_;     // who is being verified ("Verify identity" window)
    bool openVerify_ = false, openInvite_ = false, openGuide_ = false, openAbout_ = false, openCrash_ = false;
    std::string crashReport_;                  // report of an unexpected close not shown yet
    // First-time assistant
    bool openWizard_ = false;
    int wizStep_ = 0, wizShown_ = -1;
    bool wizFocus_ = false;
    std::unique_ptr<Audio> wizAudio_;          // only while the mic is being tested
    std::vector<std::string> wizIns_, wizOuts_;
    // Autocompletado
    const UiState* curState_ = nullptr;        // the state of the current frame (for the Tab call)
    Completion compList_;
    size_t compIdx_ = 0;
    std::string compBase_, compLast_;
    std::vector<CaptureSource> sources_;       // what can be shared (when pressing Share screen)
    std::vector<AudioApp> audioApps_;          // applications with sound open
    // Automatic reconnection
    bool reconnecting_ = false, wasJoined_ = false;
    int reconnectTry_ = 0;
    std::chrono::steady_clock::time_point reconnectAt_{};
    std::string lastChannel_;
    enum class PassMode { None, Register, Change, Login } passMode_ = PassMode::None, openPass_ = PassMode::None;
    std::string pass1_, pass2_, passError_;
    ImTextureID tex_ = 0;
    int texW_ = 0, texH_ = 0;
    uint32_t watchingId_ = 0;
};

// Gives the memory not in use back to the system (when minimized). On Windows it trims the
// working set, which is what Task Manager shows; when restoring, what is needed comes back
// in milliseconds.
void releaseIdleMemory() {
#ifdef _WIN32
    SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
}

// Maps a file into memory (read only). Emoji and CJK fonts weigh 10-20 MB: this way the
// system only reads from disk the pages of the glyphs actually drawn, and the memory is
// shared with the other programs using the same font. They stay open until exit.
bool mapFile(const std::string& path, void*& data, size_t& size) {
#ifdef _WIN32
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    HANDLE m = GetFileSizeEx(f, &sz) && sz.QuadPart > 0 ? CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr)
                                                        : nullptr;
    CloseHandle(f);
    if (!m) return false;
    data = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);  // the view stays alive even if the handles are closed
    size = size_t(sz.QuadPart);
    return data != nullptr;
#else
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    void* p = fstat(fd, &st) == 0 && st.st_size > 0 ? mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0)
                                                    : MAP_FAILED;
    close(fd);
    if (p == MAP_FAILED) return false;
    data = p;
    size = size_t(st.st_size);
    return true;
#endif
}

// Adds the first font in the list that exists. merge = merged with the previous one: it only provides the
// characters this one lacks (emojis, symbols, Chinese/Japanese, Korean, Indic scripts...)
bool addFont(const std::vector<std::string>& paths, float pixels, bool merge, bool color = false) {
    for (auto& p : paths) {
        void* data = nullptr;
        size_t size = 0;
        if (!mapFile(p, data, size)) continue;
        ImFontConfig c;
        c.MergeMode = merge;
        c.FontDataOwnedByAtlas = false;  // it is mapped memory, not ImGui's
        c.Flags |= ImFontFlags_NoLoadError;
        if (color) c.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LoadColor;
        if (ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, int(size), pixels, &c)) return true;
    }
    return false;
}

// System fonts. Since ImGui 1.92 each glyph is rasterized the first time it appears, so
// having many fallback fonts costs nothing at startup and uses no video memory if unused.
void loadFont(float pixels) {
    std::vector<std::string> main, emoji, symbols, cjk, korean, indic, thai, other;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    std::string w = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    main = {w + "segoeui.ttf", w + "arial.ttf"};
    emoji = {w + "seguiemj.ttf"};
    symbols = {w + "seguisym.ttf"};
    cjk = {w + "msyh.ttc", w + "YuGothR.ttc", w + "msgothic.ttc", w + "simsun.ttc"};
    korean = {w + "malgun.ttf"};
    indic = {w + "Nirmala.ttf", w + "Nirmala.ttc"};
    thai = {w + "LeelawUI.ttf", w + "leelawad.ttf", w + "tahoma.ttf"};
    other = {w + "seguihis.ttf", w + "ebrima.ttf"};
#elif defined(__APPLE__)
    main = {"/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
            "/System/Library/Fonts/Helvetica.ttc"};
    emoji = {"/System/Library/Fonts/Apple Color Emoji.ttc"};
    symbols = {"/System/Library/Fonts/Apple Symbols.ttf"};
    cjk = {"/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/STHeiti Light.ttc",
           "/System/Library/Fonts/Hiragino Sans GB.ttc"};
    korean = {"/System/Library/Fonts/AppleSDGothicNeo.ttc"};
    indic = {"/System/Library/Fonts/Kohinoor.ttc"};
    thai = {"/System/Library/Fonts/Thonburi.ttc", "/System/Library/Fonts/Supplemental/Ayuthaya.ttf"};
#else
    main = {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"};
    emoji = {"/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf", "/usr/share/fonts/noto/NotoColorEmoji.ttf",
             "/usr/share/fonts/google-noto-emoji/NotoColorEmoji.ttf", "/usr/share/fonts/noto-emoji/NotoColorEmoji.ttf",
             "/usr/share/fonts/TTF/NotoColorEmoji.ttf"};
    symbols = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
               "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf"};
    cjk = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
           "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
           "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc"};
    thai = {"/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf", "/usr/share/fonts/noto/NotoSansThai-Regular.ttf",
            "/usr/share/fonts/truetype/tlwg/Loma.ttf", "/usr/share/fonts/tlwg/Loma.otf"};
#endif
    if (!addFont(main, pixels, false)) {
        ImFontConfig fc;
        fc.SizePixels = std::max(13.f, pixels * 0.8f);
        ImGui::GetIO().Fonts->AddFontDefault(&fc);
    }
    addFont(emoji, pixels, true, true);
    addFont(symbols, pixels, true);
    addFont(cjk, pixels, true);
    addFont(korean, pixels, true);
    addFont(indic, pixels, true);
    addFont(thai, pixels, true);
    for (auto& f : other) addFont({f}, pixels, true);
}

}  // namespace

int main(int rawArgc, char** rawArgv) {
    eco::hardenDllSearch();
    eco::crash::install("eco-gui", eco::crash::clientDir());
    eco::update::cleanupOld();  // what a previous update left behind
    eco::i18n::set(eco::i18n::initial(true));  // ECO_IDIOMA, the one chosen in Settings or the system one
    eco::Utf8Args args8(rawArgc, rawArgv);  // "eco-gui server José": the nick arrives in UTF-8
    int argc = args8.argc;
    char** argv = args8.argv;
    glfwSetErrorCallback([](int, const char* d) { std::fprintf(stderr, "GLFW: %s\n", d); });
    if (!glfwInit()) return 1;
    render::windowHints();
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);  // Windows/Linux: size according to DPI

    // Interface scale. On macOS the system already works in points; sharpness on Retina screens
    // is handled by ImGui itself (it rasterizes the font at the screen's real density).
    float cs = 1.f;
    if (GLFWmonitor* m = glfwGetPrimaryMonitor()) glfwGetMonitorContentScale(m, &cs, nullptr);
    if (cs < 1.f) cs = 1.f;
#ifdef __APPLE__
    float ui = 1.f;
#else
    float ui = cs;
#endif
    g_win = glfwCreateWindow(1100, 700, T("Eco"), nullptr, nullptr);
    if (!g_win) {
        std::fprintf(stderr, T("Could not create the window\n"));
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(g_win, int(640 * ui), int(420 * ui), GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwSetWindowFocusCallback(g_win, [](GLFWwindow*, int f) { g_focused = f != 0; });

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini next to the executable
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    loadFont(17.f * ui);
    applyTheme(ui);
    // Installed before ImGui: its backend chains them with its own
    glfwSetKeyCallback(g_win, [](GLFWwindow*, int, int, int, int) { ++g_events; });
    glfwSetCharCallback(g_win, [](GLFWwindow*, unsigned) { ++g_events; });
    glfwSetMouseButtonCallback(g_win, [](GLFWwindow*, int, int, int) { ++g_events; });
    glfwSetScrollCallback(g_win, [](GLFWwindow*, double, double) { ++g_events; });
    glfwSetCursorPosCallback(g_win, [](GLFWwindow*, double, double) { ++g_events; });
    glfwSetWindowSizeCallback(g_win, [](GLFWwindow*, int, int) { ++g_events; });
    if (!render::init(g_win)) {
        std::fprintf(stderr, T("Could not start the graphics system\n"));
        return 1;
    }

    setLogSink(addLog);
    {
        App app(ui);
        if (argc >= 3) app.autoConnect(argv[1], argv[2]);
        int idle = 0;
        int lastEvents = 0;
        bool wasIconified = false;
        while (!glfwWindowShouldClose(g_win)) {
            // With activity it repaints at the screen rate; when idle it sleeps until an event arrives
            // (keyboard, mouse, a message or a frame) or until it is time to animate something (idleWait)
            bool iconified = glfwGetWindowAttrib(g_win, GLFW_ICONIFIED) != 0;
            if (!iconified && idle < 3) glfwPollEvents();  // right after an interaction: immediate response
            else glfwWaitEventsTimeout(iconified ? 2.0 : app.idleWait(g_focused));
            if (g_attention.exchange(false)) glfwRequestWindowAttention(g_win);

            bool input = g_events != lastEvents || ImGui::IsAnyMouseDown();
            lastEvents = g_events;
            {
                std::lock_guard<std::mutex> l(logMu);
                if (logDirty) input = true;
            }
            {
                std::lock_guard<std::mutex> l(frameMu);
                if (frameNew) input = true;
            }
            idle = input ? 0 : idle + 1;
            if (iconified != wasIconified) {
                wasIconified = iconified;
                if (iconified) releaseIdleMemory();  // minimized: what isn't used goes back to the system
            }
            if (iconified) continue;

            render::newFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            render::present(g_win);
        }
    }  // the client is closed here, before releasing ImGui
    setLogSink(nullptr);

    render::shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(g_win);
    glfwTerminate();
    return 0;
}

#if defined(_WIN32) && defined(_MSC_VER)
// With MSVC, a window application (no console behind) enters through WinMain; MinGW already calls main
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
