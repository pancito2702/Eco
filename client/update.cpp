// What updates need from the system: where the program is, cleaning up and restarting.
#include "../common/update.hpp"

#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace eco::update {

namespace fs = std::filesystem;

std::string exePath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof buf / sizeof buf[0]));
    if (n == 0 || n >= sizeof buf / sizeof buf[0]) return "";
    return fs::path(std::wstring(buf, n)).u8string();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) != 0) return "";
    std::error_code ec;
    return fs::canonical(buf, ec).string();
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).string();
#endif
}

std::string exeDir() {
    std::string p = exePath();
    return p.empty() ? "" : fs::u8path(p).parent_path().u8string();
}

// When installing, the running executable is renamed to .old (Windows doesn't allow deleting it while it runs)
void cleanupOld() {
    std::error_code ec;
    std::string d = exeDir();
    if (d.empty()) return;
    for (auto& e : fs::directory_iterator(fs::u8path(d), ec)) {
        auto n = e.path().filename().u8string();
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".old") == 0 && n.rfind("eco", 0) == 0) fs::remove(e.path(), ec);
        if (n.size() > 9 && n.compare(n.size() - 9, 9, ".download") == 0 && n.rfind("eco", 0) == 0) fs::remove(e.path(), ec);
        if (n.size() > 9 && n.compare(n.size() - 9, 9, ".descarga") == 0 && n.rfind("eco", 0) == 0) fs::remove(e.path(), ec);  // before 2.3
    }
}

bool relaunch() {
    std::string p = exePath();
    if (p.empty()) return false;
#ifdef _WIN32
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring exe = fs::u8path(p).wstring();
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    char* argv[] = {const_cast<char*>(p.c_str()), nullptr};
    pid_t pid;
    return posix_spawn(&pid, p.c_str(), nullptr, nullptr, argv, environ) == 0;
#endif
}

}  // namespace eco::update
