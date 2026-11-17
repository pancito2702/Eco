// Eco's encryption, built on Monocypher (X25519, EdDSA, XChaCha20-Poly1305, BLAKE2b).
//
// Greeting (in plaintext, once per connection):
//   C->S: "ECO2", u32 version, client ephemeral public key (32)
//   S->C: server ephemeral key (32), Ed25519 identity key (32),
//         signature (64) of ("eco-handshake" | version | client ephemeral | server ephemeral)
// The session keys come from X25519(ephemerals) + BLAKE2b: four different keys
// (TCP and UDP, one per direction). Being ephemeral, there is perfect forward secrecy.
// The client remembers the server identity (like SSH) to detect impersonation.
#pragma once
#include <monocypher.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace eco {

// Random bytes from the operating system (secure.cpp). Aborts if there is no secure source.
void secureRandom(uint8_t* p, size_t n);

constexpr size_t kMacSize = 16;
constexpr size_t kClientHelloSize = 4 + 4 + 32;
constexpr size_t kServerHelloSize = 32 + 32 + 64;

struct SessionKeys {
    uint8_t tcpC2S[32], tcpS2C[32], udpC2S[32], udpS2C[32];
    ~SessionKeys() { crypto_wipe(this, sizeof *this); }
};

// What the server signs: binds the signature to this specific connection
inline std::vector<uint8_t> handshakeTranscript(uint32_t version, const uint8_t cpub[32], const uint8_t spub[32]) {
    static const char label[] = "eco-handshake";
    std::vector<uint8_t> t(label, label + sizeof label);
    for (int s = 24; s >= 0; s -= 8) t.push_back(uint8_t(version >> s));
    t.insert(t.end(), cpub, cpub + 32);
    t.insert(t.end(), spub, spub + 32);
    return t;
}

inline void deriveKeys(const uint8_t shared[32], const uint8_t cpub[32], const uint8_t spub[32],
                       const uint8_t idpub[32], SessionKeys& k) {
    uint8_t msg[8 + 96], out[64];
    std::memcpy(msg + 8, cpub, 32);
    std::memcpy(msg + 40, spub, 32);
    std::memcpy(msg + 72, idpub, 32);
    std::memcpy(msg, "eco-tcp\0", 8);
    crypto_blake2b_keyed(out, 64, shared, 32, msg, sizeof msg);
    std::memcpy(k.tcpC2S, out, 32);
    std::memcpy(k.tcpS2C, out + 32, 32);
    std::memcpy(msg, "eco-udp\0", 8);
    crypto_blake2b_keyed(out, 64, shared, 32, msg, sizeof msg);
    std::memcpy(k.udpC2S, out, 32);
    std::memcpy(k.udpS2C, out + 32, 32);
    crypto_wipe(out, sizeof out);
}

inline void nonceFrom(uint64_t counter, uint8_t nonce[24]) {
    std::memset(nonce, 0, 24);
    for (int i = 0; i < 8; ++i) nonce[i] = uint8_t(counter >> (8 * i));
}

// One direction of the TCP channel. The counter is implicit: any altered,
// repeated, reordered or omitted message makes verification fail.
struct TcpCipher {
    const uint8_t* key = nullptr;
    uint64_t counter = 0;

    // Returns the message ready to send: [u32 length][ciphertext][mac]
    std::vector<uint8_t> seal(const uint8_t* plain, size_t n) {
        std::vector<uint8_t> out(4 + n + kMacSize);
        uint32_t len = uint32_t(n + kMacSize);
        for (int i = 0; i < 4; ++i) out[i] = uint8_t(len >> (24 - 8 * i));
        uint8_t nonce[24];
        nonceFrom(counter++, nonce);
        crypto_aead_lock(out.data() + 4, out.data() + 4 + n, key, nonce, nullptr, 0, plain, n);
        return out;
    }
    // Decrypts in its own buffer ([ciphertext][mac]); leaves the text in data[0..n-16)
    bool open(uint8_t* data, size_t n) {
        if (n < kMacSize + 1) return false;
        uint8_t nonce[24];
        nonceFrom(counter++, nonce);
        return crypto_aead_unlock(data, data + n - kMacSize, key, nonce, nullptr, 0, data, n - kMacSize) == 0;
    }
};

// UDP packet: [plaintext prefix][u64 counter][ciphertext][mac]. Prefix and counter are authenticated.
// Version that reuses the output buffer (no memory allocation on every packet)
inline void udpSealInto(std::vector<uint8_t>& out, const uint8_t key[32], uint64_t counter, const uint8_t* prefix,
                        size_t prefixLen, const uint8_t* plain, size_t n) {
    out.resize(prefixLen + 8 + n + kMacSize);
    if (prefixLen) std::memcpy(out.data(), prefix, prefixLen);
    for (int i = 0; i < 8; ++i) out[prefixLen + i] = uint8_t(counter >> (8 * i));
    uint8_t nonce[24];
    nonceFrom(counter, nonce);
    size_t head = prefixLen + 8;
    crypto_aead_lock(out.data() + head, out.data() + head + n, key, nonce, out.data(), head, plain, n);
}

inline std::vector<uint8_t> udpSeal(const uint8_t key[32], uint64_t counter, const uint8_t* prefix, size_t prefixLen,
                                    const uint8_t* plain, size_t n) {
    std::vector<uint8_t> out;
    udpSealInto(out, key, counter, prefix, prefixLen, plain, n);
    return out;
}

inline bool udpOpen(const uint8_t key[32], const uint8_t* pkt, size_t len, size_t prefixLen, uint64_t& counter,
                    std::vector<uint8_t>& plain) {
    size_t head = prefixLen + 8;
    if (len < head + 1 + kMacSize) return false;
    counter = 0;
    for (int i = 0; i < 8; ++i) counter |= uint64_t(pkt[prefixLen + i]) << (8 * i);
    uint8_t nonce[24];
    nonceFrom(counter, nonce);
    size_t n = len - head - kMacSize;
    plain.resize(n);
    return crypto_aead_unlock(plain.data(), pkt + head + n, key, nonce, pkt, head, pkt + head, n) == 0;
}

// Sliding window against replay of UDP packets (accepts reordering of up to 64)
struct ReplayWindow {
    uint64_t top = 0, bits = 0;
    bool started = false;
    bool accept(uint64_t c) {
        if (!started) { started = true; top = c; bits = 1; return true; }
        if (c > top) {
            uint64_t d = c - top;
            bits = d >= 64 ? 0 : bits << d;
            bits |= 1;
            top = c;
            return true;
        }
        uint64_t d = top - c;
        if (d >= 64) return false;
        uint64_t m = uint64_t(1) << d;
        if (bits & m) return false;
        bits |= m;
        return true;
    }
};

// Readable fingerprint of the server identity: "a1b2-c3d4-..."
inline std::string fingerprint(const uint8_t idpub[32]) {
    uint8_t h[16];
    crypto_blake2b(h, sizeof h, idpub, 32);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i && i % 2 == 0) s += '-';
        s += hex[h[i] >> 4];
        s += hex[h[i] & 15];
    }
    return s;
}

}  // namespace eco
