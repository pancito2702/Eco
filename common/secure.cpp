#include "secure.hpp"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace eco {

void secureRandom(uint8_t* p, size_t n) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, p, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return;
#else
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t got = 0;
        while (got < n) {
            ssize_t r = ::read(fd, p + got, n - got);
            if (r <= 0) break;
            got += size_t(r);
        }
        ::close(fd);
        if (got == n) return;
    }
#endif
    // Without secure randomness nothing can be encrypted: better to stop than to use weak keys
    std::fprintf(stderr, "Fatal error: no secure random generator\n");
    std::abort();
}

}  // namespace eco
