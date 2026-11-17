// Server persistence with SQLite (a single eco.db file).
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../common/protocol.hpp"

struct sqlite3;

namespace eco {

class Db {
public:
    ~Db();
    bool open(const std::string& path, std::string& err);

    // Binary settings (e.g. the server identity key)
    std::vector<uint8_t> blob(const std::string& key);
    void setBlob(const std::string& key, const std::vector<uint8_t>& value);

    // Accounts (passwords with Argon2id)
    struct Account {
        std::string nick;
        Role role = Role::User;
        int64_t created = 0;
    };
    std::optional<Account> account(const std::string& nick);
    // The hash (Argon2id, ~19 MiB and tens of ms) is slow: the server computes it on another thread with
    // hashPassword() and here only the results are read or saved (fast, on the main thread)
    struct Credentials {
        std::vector<uint8_t> salt, hash;
        int64_t kib = 0, passes = 0;
    };
    static constexpr int64_t kArgonKiB = 19 * 1024;  // minimum recommended by OWASP
    static constexpr int64_t kArgonPasses = 2;
    static std::vector<uint8_t> hashPassword(const std::string& pass, const std::vector<uint8_t>& salt, int64_t kib,
                                             int64_t passes);  // safe from any thread
    static std::vector<uint8_t> newSalt();
    std::optional<Credentials> credentials(const std::string& nick);
    bool createAccount(const std::string& nick, const std::vector<uint8_t>& salt, const std::vector<uint8_t>& hash,
                       Role role);
    bool setPassword(const std::string& nick, const std::vector<uint8_t>& salt, const std::vector<uint8_t>& hash);
    bool setRole(const std::string& nick, Role role);
    bool deleteAccount(const std::string& nick);  // also deletes their private messages
    void touchLogin(const std::string& nick);
    // E2E public key of the last device they logged in with (for offline private messages)
    void setPubkey(const std::string& nick, const std::vector<uint8_t>& key);
    std::vector<uint8_t> pubkey(const std::string& nick);
    std::vector<Account> accounts();

    // Canales
    std::vector<std::string> channels();
    void addChannel(const std::string& name);
    void removeChannel(const std::string& name);

    // IP bans (or /64)
    std::set<std::string> bans();
    void addBan(const std::string& ip, const std::string& nick, const std::string& by);
    bool removeBan(const std::string& ip);
    // Sanctions that survive reconnecting: "ban_cuenta" (nick), "silencio_cuenta" (nick), "silencio_ip" (IP or /64)
    struct Sanction { std::string kind, target, nick, by; int64_t at = 0; };
    std::set<std::string> sanctionTargets(const std::string& kind);  // lowercase
    void addSanction(const std::string& kind, const std::string& target, const std::string& nick, const std::string& by);
    bool removeSanction(const std::string& kind, const std::string& target);
    std::vector<Sanction> sanctions();

    // Consistent backup of the whole database (VACUUM INTO) into a new file
    bool backupTo(const std::string& path, std::string& err);

    // Mensajes
    struct Message {
        int64_t id = 0, ts = 0;
        std::string channel, from, to, text;
    };
    void addChannelMessage(const std::string& channel, const std::string& from, const std::string& text);
    // fromRegistered: the sender had an account (only then does it appear in their own history)
    void addPrivateMessage(const std::string& from, bool fromRegistered, const std::string& to, const std::string& text,
                           bool delivered);
    std::vector<Message> history(const std::string& channel, int limit);
    std::vector<Message> privates(const std::string& nick, int limit);
    // The oldest undelivered ones, at most "limit"; only those are marked as delivered
    std::vector<Message> takePending(const std::string& nick, int limit);
    int64_t pendingCount(const std::string& nick);
    int prune(int days);

    // Caps against filling the disk: beyond them the oldest are deleted
    static constexpr int64_t kMaxPending = 500;        // undelivered private messages per recipient
    static constexpr int64_t kKeepPerChannel = 20000;  // messages kept per channel
    static constexpr int64_t kKeepPerUser = 5000;      // private messages kept per recipient

private:
    bool exec(const char* sql);
    sqlite3* db_ = nullptr;
    // Trimming to the caps costs walking the index: it is done every 100 inserts, not on each one
    // (the cap is exceeded by at most 100 messages, irrelevant for the disk)
    static constexpr int kTrimEvery = 100;
    std::map<std::string, int> sinceTrim_;
};

}  // namespace eco
