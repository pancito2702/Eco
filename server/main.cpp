// eco-server: the Eco server.
#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>

#include "../common/crash.hpp"
#include "../common/utf8args.hpp"
#include "../common/i18n.hpp"

int serverMain(int argc, char** argv);

int main(int argc, char** argv) {
    eco::hardenDllSearch();
    eco::i18n::set(eco::i18n::initial(false));  // ECO_IDIOMA or the system one; --language changes it later
    eco::crash::install("eco-server", "fallos");
    if (auto r = eco::crash::unseenReport("fallos"); !r.empty())
        std::printf(eco::T("WARNING: last time the server closed unexpectedly. Report (without personal data): %s\n"), r.c_str());
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    eco::Utf8Args a(argc, argv);  // passwords and channel names with accents, in UTF-8
    int rc = serverMain(a.argc, a.argv);
#ifdef _WIN32
    // Opened with a double click (the console is ours alone): if something fails, give time to read it
    DWORD procs[2];
    if (rc != 0 && GetConsoleProcessList(procs, 2) == 1) {
        std::printf(eco::T("\nPress Enter to close..."));
        std::getchar();
    }
#endif
    return rc;
}
