// End-to-end encryption with a shared phrase.
// The phrase never leaves this computer: from it the keys (Argon2id) are derived with which
// text, voice and screen are encrypted before sending them. The server only sees encrypted data.
#pragma once
#include <cstdint>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../common/secure.hpp"

namespace eco {

class E2E {
public:
    ~E2E() { clear(); }

    bool enabled() const { return on_; }
    // serverId: server identity, used as salt (the same phrase gives different keys on each server)
    bool setPassphrase(const std::string& phrase, const uint8_t serverId[32]);
    void clear();
    std::string check();  // short code to compare with friends that everyone has the same phrase

    // Text (chat and private messages): returns marker + base64. "ad" binds the message to its sender and destination.
    std::string sealText(const std::string& plain, const std::string& ad);
    static bool isSealed(const std::string& s);
    bool openText(const std::string& sealed, const std::string& ad, std::string& plain);

    // Voice: [8 sender salt][4 counter][ciphertext][mac]
    bool sealVoice(const uint8_t* opus, size_t n, std::vector<uint8_t>& out);
    bool openVoice(const uint8_t* p, size_t n, std::vector<uint8_t>& opus);

    // Datos grandes (fotogramas): [24 nonce][cifrado][mac]
    bool sealBlob(const uint8_t* p, size_t n, uint8_t ad, std::vector<uint8_t>& out);
    bool openBlob(const uint8_t* p, size_t n, uint8_t ad, std::vector<uint8_t>& out);

    static constexpr size_t kMaxText = 1200;  // so the encrypted message fits in the server limit

private:
    std::mutex mu_;
    std::atomic<bool> on_{false};  // read without a lock by the audio and screen threads
    uint8_t textKey_[32], voiceKey_[32], blobKey_[32];
    uint8_t voiceSalt_[8];
    uint32_t voiceCtr_ = 0;
    std::map<uint64_t, ReplayWindow> voiceSeen_;  // per sender (its salt), against replays
};

}  // namespace eco
