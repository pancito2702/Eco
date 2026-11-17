// Signed updates. The Eco server itself hands them out (--updates folder), but whoever
// decides whether they are valid is the client: it only installs what comes signed with the key of whoever publishes Eco
// (update_key.hpp), is from a newer version than its own and for its same platform. A malicious server
// can, at most, not offer the update; it can't sneak in something else or go back to an older version.
//
// Manifiesto (texto, firmado entero):
//   eco-actualizacion 1
//   version 2.2.0
//   plataforma windows-x64
//   archivo eco-gui.exe <size> <BLAKE2b-256 in hex>
//   archivo ...
// Firma: EdDSA (Monocypher) sobre BLAKE2b-512("eco-update-v1\n" + manifiesto).
// Tool to publish: eco-sign (tools/sign.cpp).
#pragma once
#include <monocypher.h>

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace eco::update {

struct File {
    std::string name;
    uint64_t size = 0;
    uint8_t hash[32] = {0};
};

struct Manifest {
    std::string version, platform;
    std::vector<File> files;
};

// Platform of this executable
inline const char* platform() {
#if defined(_WIN32) && (defined(_WIN64) || defined(__x86_64__))
    return "windows-x64";
#elif defined(_WIN32)
    return "windows-x86";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x64";
#elif defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x64";
#endif
}

// A file name without paths or anything odd: letters, digits, '.', '-' and '_', not starting with '.'
inline bool safeName(const std::string& n) {
    if (n.empty() || n.size() > 64 || n[0] == '.') return false;
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
            return false;
    return true;
}

inline void digest(const std::string& manifest, uint8_t out[64]) {
    static const char tag[] = "eco-update-v1\n";
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 64);
    crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(tag), sizeof tag - 1);
    crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
    crypto_blake2b_final(&ctx, out);
}

inline bool verify(const std::string& manifest, const uint8_t sig[64], const uint8_t pub[32]) {
    uint8_t d[64];
    digest(manifest, d);
    return crypto_eddsa_check(sig, pub, d, 64) == 0;
}

inline void sign(const std::string& manifest, const uint8_t secret[64], uint8_t sig[64]) {
    uint8_t d[64];
    digest(manifest, d);
    crypto_eddsa_sign(sig, secret, d, 64);
}

inline std::string hex(const uint8_t* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += h[p[i] >> 4]; s += h[p[i] & 15]; }
    return s;
}

inline bool unhex(const std::string& s, uint8_t* out, size_t n) {
    if (s.size() != n * 2) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < n; ++i) {
        int a = v(s[2 * i]), b = v(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = uint8_t(a << 4 | b);
    }
    return true;
}

inline bool parse(const std::string& text, Manifest& m) {
    std::istringstream is(text);
    std::string line;
    bool header = false;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string k;
        ls >> k;
        if (k.empty()) continue;
        if (k == "eco-actualizacion") { std::string v; ls >> v; header = v == "1"; }
        else if (k == "version") ls >> m.version;
        else if (k == "plataforma") ls >> m.platform;
        else if (k == "archivo") {
            File f;
            std::string h;
            ls >> f.name >> f.size >> h;
            if (!safeName(f.name) || f.size == 0 || f.size > 128u * 1024 * 1024 || !unhex(h, f.hash, 32)) return false;
            m.files.push_back(f);
        }
    }
    return header && !m.version.empty() && !m.platform.empty() && !m.files.empty() && m.files.size() <= 16;
}

// Compares "x.y.z" versions: <0, 0 or >0
inline int compareVersions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& v) {
        std::vector<long> p;
        long cur = -1;
        for (char c : v) {
            if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
            else if (c == '.') { p.push_back(cur < 0 ? 0 : cur); cur = -1; }
            else break;
        }
        if (cur >= 0) p.push_back(cur);
        while (p.size() < 3) p.push_back(0);
        return p;
    };
    auto x = parts(a), y = parts(b);
    return x < y ? -1 : x > y ? 1 : 0;
}

// Folder of the running executable, path of the executable itself, delete the .old files from last time and restart
// the program (update.cpp, client only)
std::string exeDir();
std::string exePath();
void cleanupOld();
bool relaunch();

}  // namespace eco::update
