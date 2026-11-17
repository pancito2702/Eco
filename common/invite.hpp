// Invitations: everything needed to enter a server in a single line to paste.
//   eco://host:port/#f=<fingerprint>[&p=<server password>]
// The fingerprint (the server's Ed25519 identity) makes the first connection verified: if someone
// impersonates the server, the client refuses to connect. It goes after '#' so that, if someone opens it
// as a web link, it isn't sent anywhere.
#pragma once
#include <cctype>
#include <cstdint>
#include <string>

#include "protocol.hpp"

namespace eco {

struct Invite {
    std::string host, fingerprint, serverPass;  // fingerprint: 32 lowercase hex digits
    uint16_t port = kDefaultPort;
};

// "a1b2-c3d4-..." or "A1B2C3D4..." -> "a1b2c3d4..." (empty if they aren't 32 hex digits)
inline std::string normalizeFingerprint(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '-' || c == ' ' || c == ':') continue;
        if (!std::isxdigit((unsigned char)c)) return "";
        out += char(std::tolower((unsigned char)c));
    }
    return out.size() == 32 ? out : "";
}

inline std::string pctEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

inline bool pctDecode(const std::string& s, std::string& out) {
    out.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') { out += s[i]; continue; }
        if (i + 2 >= s.size() || !std::isxdigit((unsigned char)s[i + 1]) || !std::isxdigit((unsigned char)s[i + 2]))
            return false;
        out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
        i += 2;
    }
    return true;
}

inline bool isInvite(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    return a != std::string::npos && s.compare(a, 6, "eco://") == 0;
}

inline std::string makeInvite(const std::string& host, uint16_t port, const std::string& fingerprint,
                              const std::string& serverPass) {
    std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;  // IPv6 entre corchetes
    std::string s = "eco://" + h + ":" + std::to_string(port) + "/#f=" + normalizeFingerprint(fingerprint);
    if (!serverPass.empty()) s += "&p=" + pctEncode(serverPass);
    return s;
}

inline bool parseInvite(const std::string& in, Invite& out) {
    std::string s = in;
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos || s.compare(a, 6, "eco://") != 0) return false;
    s = s.substr(a + 6);
    size_t hash = s.find('#');
    std::string hostPart = s.substr(0, hash), frag = hash == std::string::npos ? "" : s.substr(hash + 1);
    size_t slash = hostPart.find('/');
    if (slash != std::string::npos) hostPart = hostPart.substr(0, slash);
    out = Invite{};
    // host[:port] or [ipv6][:port]
    std::string portStr;
    if (!hostPart.empty() && hostPart[0] == '[') {
        size_t e = hostPart.find(']');
        if (e == std::string::npos) return false;
        out.host = hostPart.substr(1, e - 1);
        if (e + 1 < hostPart.size()) {
            if (hostPart[e + 1] != ':') return false;
            portStr = hostPart.substr(e + 2);
        }
    } else {
        size_t c = hostPart.rfind(':');
        out.host = hostPart.substr(0, c);
        if (c != std::string::npos) portStr = hostPart.substr(c + 1);
    }
    if (out.host.empty() || out.host.size() > 253) return false;
    for (char ch : out.host)
        if (!(std::isalnum((unsigned char)ch) || ch == '.' || ch == '-' || ch == ':' || ch == '_')) return false;
    if (!portStr.empty()) {
        if (portStr.size() > 5 || portStr.find_first_not_of("0123456789") != std::string::npos) return false;
        int p = std::stoi(portStr);
        if (p <= 0 || p > 65535) return false;
        out.port = uint16_t(p);
    }
    // Fragmento: f=...&p=...
    size_t pos = 0;
    while (pos <= frag.size() && !frag.empty()) {
        size_t amp = frag.find('&', pos);
        std::string kv = frag.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "f") out.fingerprint = normalizeFingerprint(v);
            else if (k == "p" && !pctDecode(v, out.serverPass)) return false;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return !out.fingerprint.empty();  // without a fingerprint it isn't a valid invitation
}

}  // namespace eco
