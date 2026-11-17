// Automatic end-to-end encryption, Signal style.
//  - Each device creates an identity key (X25519) that never leaves it.
//  - Private messages: Double Ratchet. The session starts with X25519(identities) + an ephemeral key
//    (a simplified X3DH, without prekeys) and then each message uses a new key that is deleted when
//    used: forward secrecy and recovery after a compromise (each reply brings a
//    new X25519). Out-of-order or lost messages are handled with "skipped" keys.
//  - Channels: each member has a "sender key" that they hand out, encrypted one by one, to the
//    others; it is renewed every time someone joins or leaves. Each sender key brings its own
//    EdDSA pair and each message and frame is signed: the other members (who know the
//    symmetric key) can't impersonate you. It is also checked that the key belongs to whoever
//    the server says sends the message.
// The server only hands out public keys and relays data it can't read.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

#include "../common/secure.hpp"

namespace eco {

using PubKey = std::array<uint8_t, 32>;

// Path of a user configuration file (%APPDATA% or $HOME)
std::string configPath(const std::string& name);

// Protection of secrets saved on disk with the user account (DPAPI on Windows): another user
// of the computer, or whoever takes the files to another PC, can't read them. Without DPAPI they return false.
bool protectLocal(const std::string& plain, std::string& sealed);
bool unprotectLocal(const std::string& sealed, std::string& plain);

class Keys {
public:
    ~Keys();
    bool load(std::string& err);  // identity and keyring from disk (created the first time)
    const PubKey& pub() const { return pub_; }

    // Contacts: each nick's key is remembered to warn if it changes
    enum class Seen { New, Same, Changed };
    Seen remember(const std::string& server, const std::string& nick, const PubKey& key);
    bool lookup(const std::string& server, const std::string& nick, PubKey& key);
    std::string safetyCode(const PubKey& peer);  // 30 digits to compare with the other person

    // Verified contacts (the safety code was compared). If the key of a verified contact
    // changes, it gets blocked: nothing is encrypted for them and nothing of theirs is shown until it is accepted by hand.
    void setVerified(const std::string& server, const std::string& nick, const PubKey& key);
    void forgetVerified(const std::string& server, const std::string& nick);
    bool isVerified(const std::string& server, const std::string& nick, const PubKey& key);
    bool blocked(const std::string& server, const std::string& nick, const PubKey& key);

    // Private messages (Double Ratchet; also reads the old fixed-key format).
    // live: arriving now (not from history). A live message that had already been read is a replay
    // by the server: it is not opened and it is reported in *replay.
    static bool isPm(const std::string& s);
    std::string sealPm(const std::string& plain, const PubKey& peer, const std::string& ad);
    bool openPm(const std::string& sealed, const PubKey& peer, const std::string& ad, std::string& plain,
                bool live = false, bool* replay = nullptr);

    // Channel: sender key. sender = identity of whoever the server says sends it. If it isn't
    // known (nullptr), only old ownerless keys are accepted: one with an owner has to match.
    // Live (voice, screen and text with live=true) only the recent sender keys of each
    // member are valid and each text only once: that way the server can't reinject old messages.
    void rotate();                                   // creates a new sender key (and its signing pair)
    std::vector<uint8_t> wrapFor(const PubKey& peer);  // my current key, encrypted for a member
    bool unwrapFrom(const PubKey& peer, const uint8_t* p, size_t n);
    static bool isGroup(const std::string& s);
    std::string sealText(const std::string& plain, const std::string& ad);
    bool openText(const std::string& sealed, const std::string& ad, const PubKey* sender, std::string& plain,
                  bool live = false);
    bool sealVoice(const uint8_t* opus, size_t n, std::vector<uint8_t>& out);
    bool openVoice(const uint8_t* p, size_t n, const PubKey* sender, std::vector<uint8_t>& opus);
    bool sealBlob(const uint8_t* p, size_t n, uint8_t ad, std::vector<uint8_t>& out);  // always signed
    bool openBlob(const uint8_t* p, size_t n, uint8_t ad, bool sig, const PubKey* sender, std::vector<uint8_t>& out);

    // A Double Ratchet session with a contact
    struct Ratchet {
        uint8_t sid[8] = {0};   // chosen by whoever starts it; travels in every header
        bool initiator = false, confirmed = false;
        uint8_t rk[32] = {0}, dhsSec[32] = {0}, dhsPub[32] = {0}, dhr[32] = {0}, cks[32] = {0}, ckr[32] = {0};
        bool hasDhr = false, hasCks = false, hasCkr = false;
        uint32_t ns = 0, nr = 0, pn = 0;
        uint64_t lastUse = 0;
        std::map<std::array<uint8_t, 36>, std::array<uint8_t, 32>> skipped;  // (DH key, n) -> message key
        void wipe();
    };

private:
    struct Entry {
        uint8_t text[32], voice[32], blob[32];  // one subkey per use
        uint8_t sigPub[32];                     // EdDSA public key of this sender key
        PubKey owner{};                         // identity of whoever created it
        bool hasSig = false, hasOwner = false;  // old keys bring neither one nor the other
        ReplayWindow seen;                      // against replay of voice packets
    };
    struct PeerSessions {
        std::vector<Ratchet> list;  // several in case both started at the same time
        uint8_t active[8] = {0};
        bool hasActive = false;
    };
    bool pairKey(const PubKey& peer, uint8_t out[32]);
    void addKey(uint64_t id, const uint8_t key[32], const PubKey* owner, const uint8_t* sigPub, std::string* toDisk);
    void persistKey(std::string& line);  // appends the line to the keyring on disk; always outside ringMu_
    Entry* find(uint64_t id);
    static bool ownerOk(const Entry& e, const PubKey* sender);
    void noteRecent(const PubKey& owner, uint64_t id);
    bool isRecent(const Entry& e, uint64_t id) const;
    std::string ringLine(const std::string& line);           // encrypts a keyring line
    bool ringOpen(const std::string& stored, std::string& line);  // and decrypts it
    void saveContacts();
    void saveVerified();
    // Double Ratchet
    std::string ratchetSeal(const std::string& plain, const PubKey& peer, const std::string& ad);
    bool ratchetOpen(const std::vector<uint8_t>& msg, const PubKey& peer, const std::string& ad, std::string& plain);
    void loadSessions();
    bool saveSessions();  // false if it couldn't be written to disk
    // Local copy of private messages already read: their keys are deleted when used, so the server's
    // history is read from here (as Signal does with its local database). Loaded when needed.
    void loadPlainCache();
    void cachePlain(const std::string& sealed, const std::string& plain);
    bool cachedPlain(const std::string& sealed, std::string& plain);

    std::mutex mu_;
    std::string ringPath_, contactsPath_, sessionsPath_, cachePath_, verifiedPath_;  // set on load
    uint8_t secret_[32] = {0};
    PubKey pub_{};
    std::map<PubKey, std::array<uint8_t, 32>> pairs_;
    std::map<uint64_t, Entry> ring_;
    std::map<std::string, PubKey> contacts_, verified_;
    std::map<PubKey, std::vector<uint64_t>> recent_;   // last sender keys of each member
    std::set<std::array<uint8_t, 16>> seenText_;     // channel texts already received live...
    std::deque<std::array<uint8_t, 16>> seenOrder_;  // ...and in which order (the oldest are forgotten)
    // Channel keys (voice, screen, text) have their own lock: that way audio capture never
    // waits for a private message to finish saving the sessions to disk
    std::mutex ringMu_;
    std::map<PubKey, PeerSessions> sessions_;
    uint64_t useClock_ = 0;
    std::map<std::array<uint8_t, 16>, std::string> plainCache_;
    bool plainCacheLoaded_ = false;
    bool hasCur_ = false;
    uint64_t curId_ = 0;
    uint8_t curKey_[32] = {0};
    uint8_t curSign_[64] = {0}, curSignPub_[32] = {0};  // EdDSA pair of the current sender key
    uint32_t voiceCtr_ = 0;
};

std::string b64encode(const std::vector<uint8_t>& in);
bool b64decode(const char* p, size_t n, std::vector<uint8_t>& out);

}  // namespace eco
