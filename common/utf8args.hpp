// Command line arguments in UTF-8.
// On Windows, main() receives argv in the ANSI code page: "José" arrives as a Latin-1 byte and
// "Аdmin" (Cyrillic) as "?dmin". Here they are read in UTF-16 (GetCommandLineW) and converted to UTF-8.
#pragma once
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace eco {

// DLLs loaded at runtime (e.g. avrt.dll, which PortAudio opens) are only searched for in System32, never in the
// program folder or the current one: someone who leaves a DLL with that name next to the .exe (in Downloads,
// for example) can't get it to run. Call it first thing in main().
inline void hardenDllSearch() {
#ifdef _WIN32
    using Fn = BOOL(WINAPI*)(DWORD);
    if (HMODULE k = GetModuleHandleW(L"kernel32.dll"))
        if (auto f = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(k, "SetDefaultDllDirectories"))))
            f(0x00000800);  // LOAD_LIBRARY_SEARCH_SYSTEM32
    SetDllDirectoryW(L"");  // also removes the current folder
#endif
}

struct Utf8Args {
    std::vector<std::string> store;
    std::vector<char*> ptrs;
    int argc = 0;
    char** argv = nullptr;

    Utf8Args(int ac, char** av) {
#ifdef _WIN32
        int n = 0;
        if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
            for (int i = 0; i < n; ++i) {
                int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
                std::string s(len > 0 ? size_t(len - 1) : 0, '\0');
                if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, nullptr, nullptr);
                store.push_back(std::move(s));
            }
            LocalFree(w);
        }
#endif
        if (store.empty())
            for (int i = 0; i < ac; ++i) store.emplace_back(av[i]);
        for (auto& s : store) ptrs.push_back(&s[0]);
        ptrs.push_back(nullptr);
        argc = int(store.size());
        argv = ptrs.data();
    }
};

}  // namespace eco
