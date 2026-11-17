// Public key the Eco versions are signed with. Clients only install updates signed
// with its private key.
//
// It isn't in the code: whoever builds Eco puts their own. CMake takes it from -DECO_UPDATE_PUBLIC_KEY=<64 hex> or,
// otherwise, from local_data_DO_NOT_DISTRIBUTE/update_public_key.txt (written by "eco-sign new-key"). With neither of
// the two, all zeros: automatic updates are disabled and the client doesn't accept any.
#pragma once
#include <cstdint>

#ifndef ECO_UPDATE_PUBLIC_KEY
#define ECO_UPDATE_PUBLIC_KEY ""
#endif

namespace eco {
namespace update_key_detail {
constexpr uint8_t nibble(char c) {
    return c >= '0' && c <= '9' ? uint8_t(c - '0') : c >= 'a' && c <= 'f' ? uint8_t(c - 'a' + 10)
                                                    : c >= 'A' && c <= 'F' ? uint8_t(c - 'A' + 10) : 0;
}
constexpr bool valid(const char* s) {
    int n = 0;
    for (; s[n]; ++n) {
        char c = s[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return n == 64;
}
constexpr uint8_t byteAt(const char* s, int i) {
    return valid(s) ? uint8_t(nibble(s[2 * i]) << 4 | nibble(s[2 * i + 1])) : 0;
}
}  // namespace update_key_detail

#define ECO_KB(i) update_key_detail::byteAt(ECO_UPDATE_PUBLIC_KEY, i)
constexpr uint8_t kUpdateKey[32] = {ECO_KB(0),  ECO_KB(1),  ECO_KB(2),  ECO_KB(3),  ECO_KB(4),  ECO_KB(5),  ECO_KB(6),
                                    ECO_KB(7),  ECO_KB(8),  ECO_KB(9),  ECO_KB(10), ECO_KB(11), ECO_KB(12), ECO_KB(13),
                                    ECO_KB(14), ECO_KB(15), ECO_KB(16), ECO_KB(17), ECO_KB(18), ECO_KB(19), ECO_KB(20),
                                    ECO_KB(21), ECO_KB(22), ECO_KB(23), ECO_KB(24), ECO_KB(25), ECO_KB(26), ECO_KB(27),
                                    ECO_KB(28), ECO_KB(29), ECO_KB(30), ECO_KB(31)};
#undef ECO_KB
static_assert(sizeof(ECO_UPDATE_PUBLIC_KEY) == 1 || update_key_detail::valid(ECO_UPDATE_PUBLIC_KEY),
              "ECO_UPDATE_PUBLIC_KEY has to be the public key in hexadecimal (64 characters)");
}  // namespace eco
