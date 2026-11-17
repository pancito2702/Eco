// Eco's version. The number is changed in a single place: project(Eco VERSION x.y.z) in CMakeLists.txt,
// which passes it to all the code as ECO_VERSION (and to the .exe properties on Windows and the .app on macOS).
// It is different from kProtoVersion (protocol.hpp), which only changes when client and server stop understanding each other.
#pragma once
#include <cctype>
#include <string>

#ifndef ECO_VERSION
#define ECO_VERSION "dev"  // built without CMake
#endif

namespace eco {

constexpr const char* kVersion = ECO_VERSION;

// The version the other end reports: only digits, letters, dots and dashes, and short (it is shown on screen)
inline std::string cleanVersion(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (out.size() >= 16) break;
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') out += c;
    }
    return out;
}

}  // namespace eco
