// eco-client: the Eco client. With a double click (no arguments) it asks for server and nick.
#include <iostream>
#include "../common/i18n.hpp"
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../common/crash.hpp"
#include "../common/utf8args.hpp"

int clientMain(int argc, char** argv);
using eco::i18n::T;

namespace {
std::string ask(const std::string& q) {
    std::cout << q << std::flush;
    std::string s;
    std::getline(std::cin, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
}  // namespace

int main(int rawArgc, char** rawArgv) {
    eco::hardenDllSearch();
    eco::i18n::set(eco::i18n::initial(true));
    eco::crash::install("eco-client", eco::crash::clientDir());
    if (auto r = eco::crash::unseenReport(eco::crash::clientDir()); !r.empty())
        std::cout << T("Warning: Eco closed unexpectedly last time. There is a report (without personal data) in\n  ")
                  << r << T("\nIf you want to help fix it, send it to whoever gave you Eco.\n\n");
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    eco::Utf8Args args8(rawArgc, rawArgv);  // nicks and passwords with accents arrive fine
    int argc = args8.argc;
    char** argv = args8.argv;
    if (argc > 1) return clientMain(argc, argv);
    std::cout << T("Eco - lightweight encrypted voice chat\n\n");
    std::string host = ask(T("Server or invitation (paste here the one you were given): ")), nick = ask(T("Your nick: "));
    if (host.empty() || nick.empty()) return 1;
    std::vector<std::string> args = {argv[0], host, nick};
    std::vector<char*> av;
    for (auto& a : args) av.push_back(&a[0]);
    int rc = clientMain(int(av.size()), av.data());
    ask(T("\nPress Enter to close..."));
    return rc;
}
