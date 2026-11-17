// Unexpected close reports. If Eco crashes, it leaves a text file with just enough to find the
// bug: program, version, system, the type of error and at which point of the code (module + offset).
// It has no personal data: no messages, no nicks, no passwords, no paths with your user name.
// It stays on your computer; sending it is up to you (Eco never sends it by itself).
//   Clients: %APPDATA%\eco_fallos (Windows) or ~/.eco_fallos      Server: ./fallos
#pragma once
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#include "version.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#endif

namespace eco::crash {

inline std::string clientDir() {
#ifdef _WIN32
    const char* d = std::getenv("APPDATA");
    return d ? std::string(d) + "\\eco_fallos" : "eco_fallos";
#else
    const char* d = std::getenv("HOME");
    return d ? std::string(d) + "/.eco_fallos" : "eco_fallos";
#endif
}

namespace detail {
inline char g_path[1024];       // report file, prepared beforehand (no memory is allocated during a crash)
inline char g_program[64];

inline void appendHex(char*& p, char* end, unsigned long long v) {
    char tmp[17];
    int n = 0;
    do { tmp[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v && n < 16);
    if (p + 2 < end) { *p++ = '0'; *p++ = 'x'; }
    while (n > 0 && p < end) *p++ = tmp[--n];
}
inline void appendStr(char*& p, char* end, const char* s) {
    while (*s && p < end) *p++ = *s++;
}

#ifdef _WIN32
// "eco-gui.exe+0x1a2b3": module (only the name, without the path) and offset, which doesn't change with ASLR
inline void appendWhere(char*& p, char* end, DWORD64 addr) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &m) && m) {
        char full[MAX_PATH];
        DWORD n = GetModuleFileNameA(m, full, MAX_PATH);
        if (n) {
            const char* base = full + n;
            while (base > full && base[-1] != '\\' && base[-1] != '/') --base;
            std::snprintf(name, sizeof name, "%s", base);
        }
        appendStr(p, end, name);
        appendStr(p, end, "+");
        appendHex(p, end, addr - reinterpret_cast<DWORD64>(m));
    } else {
        appendHex(p, end, addr);
    }
}

inline LONG WINAPI onException(EXCEPTION_POINTERS* ep) {
    static char buf[8192];
    char* p = buf;
    char* end = buf + sizeof buf - 2;
    appendStr(p, end, "Eco crash report\r\nProgram: ");
    appendStr(p, end, g_program);
    appendStr(p, end, "\r\nVersion: ");
    appendStr(p, end, kVersion);
    OSVERSIONINFOW vi{sizeof(vi)};
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (auto f = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) {
        f(&vi);
        char v[64];
        std::snprintf(v, sizeof v, "\r\nWindows: %lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
        appendStr(p, end, v);
    }
    appendStr(p, end, "\r\nError: ");
    appendHex(p, end, ep->ExceptionRecord->ExceptionCode);
    appendStr(p, end, " en ");
    appendWhere(p, end, reinterpret_cast<DWORD64>(ep->ExceptionRecord->ExceptionAddress));
    appendStr(p, end, "\r\nPila:\r\n");
#if defined(_M_X64) || defined(__x86_64__)
    CONTEXT ctx = *ep->ContextRecord;
    for (int i = 0; i < 40 && ctx.Rip; ++i) {
        appendStr(p, end, "  ");
        appendWhere(p, end, ctx.Rip);
        appendStr(p, end, "\r\n");
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
        if (!fn) {  // leaf function: the return address is at the top of the stack
            if (IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        } else {
            void* handler = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler, &frame, nullptr);
        }
    }
#endif
    HANDLE f = CreateFileA(g_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, buf, DWORD(p - buf), &w, nullptr);
        CloseHandle(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;  // and let Windows close the program as always
}
#else
inline void onSignal(int sig, siginfo_t* si, void*) {
    char buf[4096];
    char* p = buf;
    char* end = buf + sizeof buf - 2;
    appendStr(p, end, "Eco crash report\nProgram: ");
    appendStr(p, end, g_program);
    appendStr(p, end, "\nVersion: ");
    appendStr(p, end, kVersion);
    appendStr(p, end, "\nSenal: ");
    appendHex(p, end, (unsigned long long)sig);
    appendStr(p, end, "\nDireccion: ");
    appendHex(p, end, (unsigned long long)(uintptr_t)(si ? si->si_addr : nullptr));
    appendStr(p, end, "\n");
    int fd = ::open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        ssize_t r = ::write(fd, buf, size_t(p - buf));
        (void)r;
#if defined(__GLIBC__)
        void* frames[40];
        int n = backtrace(frames, 40);
        backtrace_symbols_fd(frames, n, fd);
#endif
        ::close(fd);
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif
}  // namespace detail

// Call at the beginning of main(). dir: where to save the reports.
inline void install(const char* program, const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    char stamp[32];
    auto t = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&t));
    std::snprintf(detail::g_program, sizeof detail::g_program, "%s", program);
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    std::snprintf(detail::g_path, sizeof detail::g_path, "%s%ceco_fallo_%s_%s.txt", dir.c_str(), sep, program, stamp);
#ifdef _WIN32
    SetUnhandledExceptionFilter(detail::onException);
#else
    struct sigaction sa {};
    sa.sa_sigaction = detail::onSignal;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(s, &sa, nullptr);
#endif
    // Uncaught C++ exception: note what it was and close as always
    std::set_terminate([] {
        std::string what = "desconocida";
        if (auto e = std::current_exception()) {
            try { std::rethrow_exception(e); } catch (const std::exception& x) { what = x.what(); } catch (...) {}
        }
        std::ofstream f(detail::g_path);
        f << "Eco crash report\nProgram: " << detail::g_program << "\nVersion: " << kVersion
          << "\nUncaught C++ exception: " << what.substr(0, 200) << "\n";
        f.close();
        std::abort();
    });
}

// Report of a close not shown yet (path), or "". Marks it as seen.
inline std::string unseenReport(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path d = fs::u8path(dir), mark = d / "visto.txt";
    std::string last;
    { std::ifstream m(mark); std::getline(m, last); }
    // The names carry program and date: compared by the date (what comes after the last '_')
    auto stampOf = [](const std::string& n) { auto u = n.rfind('_'); return u == std::string::npos ? n : n.substr(u); };
    std::string newest;
    for (auto& e : fs::directory_iterator(d, ec)) {
        std::string n = e.path().filename().u8string();
        if (n.rfind("eco_fallo_", 0) == 0 && (newest.empty() || stampOf(n) > stampOf(newest))) newest = n;
    }
    if (newest.empty() || (!last.empty() && stampOf(newest) <= stampOf(last))) return "";
    std::ofstream(mark) << newest << "\n";
    return (d / newest).u8string();
}

}  // namespace eco::crash
