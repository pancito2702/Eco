#include "db.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <ctime>
#include <cstdlib>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "../common/secure.hpp"

namespace eco {

namespace {

int64_t now() { return int64_t(std::time(nullptr)); }

// Minimal RAII wrapper for prepared statements
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt& bind(int i, const std::string& v) { sqlite3_bind_text(s_, i, v.data(), int(v.size()), SQLITE_TRANSIENT); return *this; }
    Stmt& bind(int i, int64_t v) { sqlite3_bind_int64(s_, i, v); return *this; }
    Stmt& bind(int i, const std::vector<uint8_t>& v) { sqlite3_bind_blob(s_, i, v.data(), int(v.size()), SQLITE_TRANSIENT); return *this; }
    Stmt& null(int i) { sqlite3_bind_null(s_, i); return *this; }
    bool step() { return s_ && sqlite3_step(s_) == SQLITE_ROW; }
    bool run() { return s_ && sqlite3_step(s_) == SQLITE_DONE; }
    int64_t i64(int c) { return sqlite3_column_int64(s_, c); }
    std::string str(int c) {
        auto p = sqlite3_column_text(s_, c);
        return p ? std::string((const char*)p, size_t(sqlite3_column_bytes(s_, c))) : std::string();
    }
    std::vector<uint8_t> blob(int c) {
        auto p = (const uint8_t*)sqlite3_column_blob(s_, c);
        return p ? std::vector<uint8_t>(p, p + sqlite3_column_bytes(s_, c)) : std::vector<uint8_t>();
    }

private:
    sqlite3_stmt* s_ = nullptr;
};

}  // namespace

// Argon2id: 19 MiB and 2 passes (minimum recommended by OWASP). The parameters are saved with each
// account so they can be raised in the future without invalidating passwords already created.
std::vector<uint8_t> Db::hashPassword(const std::string& pass, const std::vector<uint8_t>& salt, int64_t kib,
                                      int64_t passes) {
    std::vector<uint8_t> out(32);
    if (kib < 8 || kib > 1024 * 1024 || passes < 1 || passes > 64 || salt.empty()) return {};
    void* work = std::malloc(size_t(kib) * 1024);
    if (!work) return {};
    crypto_argon2_config cfg{CRYPTO_ARGON2_ID, uint32_t(kib), uint32_t(passes), 1};
    crypto_argon2_inputs in{reinterpret_cast<const uint8_t*>(pass.data()), salt.data(), uint32_t(pass.size()),
                            uint32_t(salt.size())};
    crypto_argon2(out.data(), 32, work, cfg, in, crypto_argon2_no_extras);
    crypto_wipe(work, size_t(kib) * 1024);  // don't leave remains of the password in freed memory
    std::free(work);
#ifdef __GLIBC__
    malloc_trim(0);  // give the temporary 19 MiB back to the system: the server goes back to using ~5 MB
#endif
    return out;
}

std::vector<uint8_t> Db::newSalt() {
    std::vector<uint8_t> s(16);
    secureRandom(s.data(), s.size());
    return s;
}

Db::~Db() {
    if (db_) {
        exec("PRAGMA optimize;");  // updates the planner statistics (cheap)
        sqlite3_close(db_);
    }
}

bool Db::exec(const char* sql) { return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

bool Db::open(const std::string& path, std::string& err) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        err = db_ ? sqlite3_errmsg(db_) : "out of memory";
        return false;
    }
    sqlite3_busy_timeout(db_, 2000);
    bool ok = exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;") &&
              exec(R"(
        CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value BLOB);
        CREATE TABLE IF NOT EXISTS accounts(
            nick TEXT PRIMARY KEY COLLATE NOCASE,
            salt BLOB NOT NULL, hash BLOB NOT NULL, mem_kib INTEGER NOT NULL, passes INTEGER NOT NULL,
            role INTEGER NOT NULL DEFAULT 0,
            created INTEGER NOT NULL, last_login INTEGER);
        CREATE TABLE IF NOT EXISTS channels(
            name TEXT PRIMARY KEY COLLATE NOCASE, pos INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS bans(
            ip TEXT PRIMARY KEY, nick TEXT, banned_by TEXT, at INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS sanctions(
            kind TEXT NOT NULL, target TEXT NOT NULL COLLATE NOCASE, nick TEXT, by_nick TEXT, at INTEGER NOT NULL,
            PRIMARY KEY(kind, target));
        CREATE TABLE IF NOT EXISTS messages(
            id INTEGER PRIMARY KEY, ts INTEGER NOT NULL,
            channel TEXT COLLATE NOCASE, from_nick TEXT NOT NULL, to_nick TEXT COLLATE NOCASE,
            text TEXT NOT NULL, delivered INTEGER NOT NULL DEFAULT 1, from_reg INTEGER NOT NULL DEFAULT 0);
        CREATE INDEX IF NOT EXISTS msg_chan ON messages(channel, id) WHERE to_nick IS NULL;
        CREATE INDEX IF NOT EXISTS msg_to ON messages(to_nick, id) WHERE to_nick IS NOT NULL;
        DROP INDEX IF EXISTS msg_from;
        CREATE INDEX IF NOT EXISTS msg_from_nc ON messages(from_nick COLLATE NOCASE, id) WHERE to_nick IS NOT NULL;
    )");
    exec("PRAGMA temp_store=MEMORY;");
    // Small page cache (512 KiB instead of 2 MiB): the server barely reads, almost everything is
    // inserts; this way it uses less RAM on a VPS without a noticeable speed difference
    exec("PRAGMA cache_size=-512;");
    if (!ok) err = sqlite3_errmsg(db_);
    else exec("ALTER TABLE accounts ADD COLUMN pubkey BLOB");  // databases from previous versions; simply fails if it already exists
    return ok;
}

// ---------- Ajustes ----------
std::vector<uint8_t> Db::blob(const std::string& key) {
    Stmt s(db_, "SELECT value FROM settings WHERE key=?");
    s.bind(1, key);
    return s.step() ? s.blob(0) : std::vector<uint8_t>();
}

void Db::setBlob(const std::string& key, const std::vector<uint8_t>& value) {
    Stmt s(db_, "INSERT OR REPLACE INTO settings(key, value) VALUES(?,?)");
    s.bind(1, key).bind(2, value).run();
}

// ---------- Cuentas ----------
std::optional<Db::Account> Db::account(const std::string& nick) {
    Stmt s(db_, "SELECT nick, role, created FROM accounts WHERE nick=?");
    s.bind(1, nick);
    if (!s.step()) return std::nullopt;
    return Account{s.str(0), Role(std::clamp<int64_t>(s.i64(1), 0, 2)), s.i64(2)};
}

std::optional<Db::Credentials> Db::credentials(const std::string& nick) {
    Stmt s(db_, "SELECT salt, hash, mem_kib, passes FROM accounts WHERE nick=?");
    s.bind(1, nick);
    if (!s.step()) return std::nullopt;
    return Credentials{s.blob(0), s.blob(1), s.i64(2), s.i64(3)};
}

bool Db::createAccount(const std::string& nick, const std::vector<uint8_t>& salt, const std::vector<uint8_t>& hash,
                       Role role) {
    if (hash.size() != 32) return false;
    Stmt s(db_, "INSERT INTO accounts(nick, salt, hash, mem_kib, passes, role, created) VALUES(?,?,?,?,?,?,?)");
    return s.bind(1, nick).bind(2, salt).bind(3, hash).bind(4, kArgonKiB).bind(5, kArgonPasses)
        .bind(6, int64_t(role)).bind(7, now()).run();
}

bool Db::setPassword(const std::string& nick, const std::vector<uint8_t>& salt, const std::vector<uint8_t>& hash) {
    if (hash.size() != 32) return false;
    Stmt s(db_, "UPDATE accounts SET salt=?, hash=?, mem_kib=?, passes=? WHERE nick=?");
    return s.bind(1, salt).bind(2, hash).bind(3, kArgonKiB).bind(4, kArgonPasses).bind(5, nick).run() &&
           sqlite3_changes(db_) > 0;
}

bool Db::setRole(const std::string& nick, Role role) {
    Stmt s(db_, "UPDATE accounts SET role=? WHERE nick=?");
    return s.bind(1, int64_t(role)).bind(2, nick).run() && sqlite3_changes(db_) > 0;
}

bool Db::deleteAccount(const std::string& nick) {
    Stmt s(db_, "DELETE FROM accounts WHERE nick=?");
    if (!s.bind(1, nick).run() || sqlite3_changes(db_) == 0) return false;
    // Two statements instead of an OR: that way each one uses its index
    Stmt m(db_, "DELETE FROM messages WHERE to_nick=?");
    m.bind(1, nick).run();
    Stmt f(db_, "DELETE FROM messages WHERE to_nick IS NOT NULL AND from_nick=? COLLATE NOCASE");
    f.bind(1, nick).run();
    return true;
}

void Db::touchLogin(const std::string& nick) {
    Stmt s(db_, "UPDATE accounts SET last_login=? WHERE nick=?");
    s.bind(1, now()).bind(2, nick).run();
}

void Db::setPubkey(const std::string& nick, const std::vector<uint8_t>& key) {
    Stmt s(db_, "UPDATE accounts SET pubkey=? WHERE nick=?");
    s.bind(1, key).bind(2, nick).run();
}

std::vector<uint8_t> Db::pubkey(const std::string& nick) {
    Stmt s(db_, "SELECT pubkey FROM accounts WHERE nick=?");
    s.bind(1, nick);
    return s.step() ? s.blob(0) : std::vector<uint8_t>();
}

std::vector<Db::Account> Db::accounts() {
    std::vector<Account> v;
    Stmt s(db_, "SELECT nick, role, created FROM accounts ORDER BY role DESC, nick");
    while (s.step()) v.push_back({s.str(0), Role(std::clamp<int64_t>(s.i64(1), 0, 2)), s.i64(2)});
    return v;
}

// ---------- Canales ----------
std::vector<std::string> Db::channels() {
    std::vector<std::string> v;
    Stmt s(db_, "SELECT name FROM channels ORDER BY pos");
    while (s.step()) v.push_back(s.str(0));
    return v;
}

void Db::addChannel(const std::string& name) {
    Stmt s(db_, "INSERT OR IGNORE INTO channels(name, pos) VALUES(?, (SELECT IFNULL(MAX(pos),0)+1 FROM channels))");
    s.bind(1, name).run();
}

void Db::removeChannel(const std::string& name) {
    Stmt a(db_, "DELETE FROM channels WHERE name=?");
    a.bind(1, name).run();
    Stmt b(db_, "DELETE FROM messages WHERE channel=? AND to_nick IS NULL");
    b.bind(1, name).run();
}

// ---------- Baneos ----------
std::set<std::string> Db::bans() {
    std::set<std::string> v;
    Stmt s(db_, "SELECT ip FROM bans");
    while (s.step()) v.insert(s.str(0));
    return v;
}

void Db::addBan(const std::string& ip, const std::string& nick, const std::string& by) {
    Stmt s(db_, "INSERT OR REPLACE INTO bans(ip, nick, banned_by, at) VALUES(?,?,?,?)");
    s.bind(1, ip).bind(2, nick).bind(3, by).bind(4, now()).run();
}

bool Db::removeBan(const std::string& ip) {
    Stmt s(db_, "DELETE FROM bans WHERE ip=?");
    return s.bind(1, ip).run() && sqlite3_changes(db_) > 0;
}

std::set<std::string> Db::sanctionTargets(const std::string& kind) {
    std::set<std::string> v;
    Stmt s(db_, "SELECT lower(target) FROM sanctions WHERE kind=?");
    s.bind(1, kind);
    while (s.step()) v.insert(s.str(0));
    return v;
}

void Db::addSanction(const std::string& kind, const std::string& target, const std::string& nick, const std::string& by) {
    Stmt s(db_, "INSERT OR REPLACE INTO sanctions(kind, target, nick, by_nick, at) VALUES(?,?,?,?,?)");
    s.bind(1, kind).bind(2, target).bind(3, nick).bind(4, by).bind(5, now()).run();
}

bool Db::removeSanction(const std::string& kind, const std::string& target) {
    Stmt s(db_, "DELETE FROM sanctions WHERE kind=? AND target=?");
    return s.bind(1, kind).bind(2, target).run() && sqlite3_changes(db_) > 0;
}

std::vector<Db::Sanction> Db::sanctions() {
    std::vector<Sanction> v;
    Stmt s(db_, "SELECT kind, target, nick, by_nick, at FROM sanctions ORDER BY at");
    while (s.step()) v.push_back({s.str(0), s.str(1), s.str(2), s.str(3), s.i64(4)});
    return v;
}

bool Db::backupTo(const std::string& path, std::string& err) {
    Stmt s(db_, "VACUUM INTO ?");
    s.bind(1, path);
    if (s.run()) return true;
    err = sqlite3_errmsg(db_);
    return false;
}

// ---------- Mensajes ----------
void Db::addChannelMessage(const std::string& channel, const std::string& from, const std::string& text) {
    Stmt s(db_, "INSERT INTO messages(ts, channel, from_nick, text) VALUES(?,?,?,?)");
    s.bind(1, now()).bind(2, channel).bind(3, from).bind(4, text).run();
    // Cap per channel (uses the msg_chan index), every kTrimEvery messages
    if (++sinceTrim_["c|" + channel] < kTrimEvery) return;
    sinceTrim_.erase("c|" + channel);
    Stmt t(db_, "DELETE FROM messages WHERE channel=?1 AND to_nick IS NULL AND id <= (SELECT id FROM messages "
                "WHERE channel=?1 AND to_nick IS NULL ORDER BY id DESC LIMIT 1 OFFSET ?2)");
    t.bind(1, channel).bind(2, kKeepPerChannel).run();
}

void Db::addPrivateMessage(const std::string& from, bool fromRegistered, const std::string& to, const std::string& text,
                           bool delivered) {
    Stmt s(db_, "INSERT INTO messages(ts, from_nick, to_nick, text, delivered, from_reg) VALUES(?,?,?,?,?,?)");
    s.bind(1, now()).bind(2, from).bind(3, to).bind(4, text).bind(5, int64_t(delivered ? 1 : 0))
        .bind(6, int64_t(fromRegistered ? 1 : 0)).run();
    // Cap per recipient: only the ones already delivered are deleted (pending ones already have their limit)
    if (++sinceTrim_["p|" + to] < kTrimEvery) return;
    sinceTrim_.erase("p|" + to);
    Stmt t(db_, "DELETE FROM messages WHERE to_nick=?1 AND delivered=1 AND id <= (SELECT id FROM messages "
                "WHERE to_nick=?1 ORDER BY id DESC LIMIT 1 OFFSET ?2)");
    t.bind(1, to).bind(2, kKeepPerUser).run();
}

int64_t Db::pendingCount(const std::string& nick) {
    Stmt s(db_, "SELECT COUNT(*) FROM messages WHERE to_nick=? AND delivered=0");
    s.bind(1, nick);
    return s.step() ? s.i64(0) : 0;
}

static std::vector<Db::Message> readMessages(Stmt& s) {
    std::vector<Db::Message> v;
    while (s.step()) v.push_back({s.i64(0), s.i64(1), s.str(2), s.str(3), s.str(4), s.str(5)});
    std::reverse(v.begin(), v.end());  // from oldest to newest
    return v;
}

std::vector<Db::Message> Db::history(const std::string& channel, int limit) {
    Stmt s(db_, "SELECT id, ts, IFNULL(channel,''), from_nick, IFNULL(to_nick,''), text FROM messages "
                "WHERE channel=? AND to_nick IS NULL ORDER BY id DESC LIMIT ?");
    s.bind(1, channel).bind(2, int64_t(limit));
    return readMessages(s);
}

std::vector<Db::Message> Db::privates(const std::string& nick, int limit) {
    // UNION instead of OR: SQLite uses an index on each half instead of scanning the whole table
    Stmt s(db_, "SELECT id, ts, '', from_nick, to_nick, text FROM messages WHERE to_nick=?1 "
                "UNION SELECT id, ts, '', from_nick, to_nick, text FROM messages "
                "WHERE to_nick IS NOT NULL AND from_nick=?1 COLLATE NOCASE AND from_reg=1 "
                "ORDER BY id DESC LIMIT ?2");
    s.bind(1, nick).bind(2, int64_t(limit));
    return readMessages(s);
}

std::vector<Db::Message> Db::takePending(const std::string& nick, int limit) {
    // Oldest first; readMessages reverses the order, so they are requested backwards
    Stmt s(db_, "SELECT id, ts, '', from_nick, to_nick, text FROM messages "
                "WHERE to_nick=? AND delivered=0 ORDER BY id ASC LIMIT ?");
    s.bind(1, nick).bind(2, int64_t(limit));
    auto v = readMessages(s);
    std::reverse(v.begin(), v.end());
    if (v.empty()) return v;
    int64_t last = 0;
    for (auto& m : v) last = std::max(last, m.id);
    Stmt u(db_, "UPDATE messages SET delivered=1 WHERE to_nick=? AND delivered=0 AND id<=?");
    u.bind(1, nick).bind(2, last).run();
    return v;
}

int Db::prune(int days) {
    if (days <= 0) return 0;
    Stmt s(db_, "DELETE FROM messages WHERE ts < ? AND delivered=1");
    s.bind(1, now() - int64_t(days) * 86400).run();
    return sqlite3_changes(db_);
}

}  // namespace eco
