// Server fuzzing: many "raw" clients send it valid encrypted messages but with random
// or malformed content (of every type, before and after identifying), commands with odd data and
// garbage UDP packets or encrypted with random content. Meanwhile, a watchdog checks every half second that
// the server keeps answering (Ping/Pong). At the end, that logging in still works.
//   eco-server --port 19989 --db fuzz.db --backups 0      and then      eco-test-fuzz 19989 [seconds]
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>

#include "raw_client.hpp"

using namespace eco;
using eco::test::Raw;
using clk = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_sent{0}, g_udp{0}, g_reconnects{0};

// Content "with shape": strings with lengths that don't add up, extreme numbers, broken UTF-8...
std::vector<uint8_t> payload(std::mt19937& rng) {
    std::vector<uint8_t> p;
    int mode = int(rng() % 6);
    size_t n = rng() % (mode == 5 ? 4000 : 300);
    switch (mode) {
        case 0:  // aleatorio puro
            p.resize(n);
            for (auto& b : p) b = uint8_t(rng());
            break;
        case 1: {  // strings with a lying length
            Writer w;
            for (int i = 0; i < 4; ++i) {
                w.u16(uint16_t(rng() % 3 == 0 ? 65535 : rng() % 64));
                for (size_t j = 0, m = rng() % 40; j < m; ++j) w.u8(uint8_t(rng()));
            }
            p = w.buf;
            break;
        }
        case 2: {  // "bytes" blocks with huge u32
            Writer w;
            w.u32(uint32_t(rng()));
            w.u32(rng() % 2 ? 0xFFFFFFFFu : uint32_t(rng() % 100));
            for (size_t j = 0; j < n % 64; ++j) w.u8(uint8_t(rng()));
            p = w.buf;
            break;
        }
        case 3:  // empty
            break;
        case 4: {  // broken UTF-8, control characters, bidi
            static const char* bits[] = {"\xff", "\xc3", "\xe2\x80\xae", "\x1b[2J", "\r\n", "\0", "Ádmin", "\xf0\x9f\x98"};
            Writer w;
            std::string s;
            for (int i = 0; i < 20; ++i) s += bits[rng() % 8];
            w.str(s);
            w.str(s);
            w.str(s);
            p = w.buf;
            break;
        }
        default:  // grande
            p.resize(n);
            for (auto& b : p) b = uint8_t(rng());
    }
    return p;
}

// Real commands with odd data
std::vector<uint8_t> command(std::mt19937& rng) {
    static const char* cmds[] = {"pm", "kick", "ban", "unban", "bans", "move", "mute", "unmute", "mkchan", "rmchan",
                                 "role", "login", "register", "passwd", "history", "pmhistory", "accounts", "delaccount",
                                 "resetpass", "who", "help", "pubkey", "", "x"};
    static const char* args[] = {"Vigia", "fz1", "", " ", "\xff\xfe", "Lobby", "admin", "../../etc/passwd", "CON",
                                 "%s%s%n", "999999999999999999", "-1", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                 "\x01sellado", "Ｌｏｂｂｙ", "\xe2\x80\xae"};
    std::string line = cmds[rng() % (sizeof cmds / sizeof *cmds)];
    for (int i = 0, m = int(rng() % 4); i < m; ++i) line += std::string(" ") + args[rng() % (sizeof args / sizeof *args)];
    Writer w;
    w.str(line);
    return w.buf;
}

void fuzzer(int idx, uint16_t port) {
    std::mt19937 rng(1234u + unsigned(idx));
    while (!g_stop) {
        Raw r;
        bool preauth = idx % 4 == 3;  // one in four, without identifying
        // Each connection from a different loopback IP: otherwise the per-IP connection brake (rightly)
        // would allow testing very little, because the server cuts the connection at the first malformed message
        std::string from = "127." + std::to_string(1 + rng() % 250) + "." + std::to_string(rng() % 256) + "." +
                           std::to_string(1 + rng() % 250);
        bool ok = preauth ? r.handshake(port, from) : r.connectTo("fz" + std::to_string(idx) + "_" + std::to_string(rng() % 100000), port, from);
        if (!ok) { std::this_thread::sleep_for(std::chrono::milliseconds(300)); ++g_reconnects; continue; }
        // Whatever the server answers is read (otherwise its queue grows and it cuts us off, which is also fine, but less)
        std::thread drain([&] {
            std::vector<uint8_t> m;
            while (r.readMsg(m)) {}
        });
        for (int i = 0; i < 400 && !g_stop; ++i) {
            uint8_t type;
            int pick = int(rng() % 10);
            if (pick < 5) type = uint8_t(1 + rng() % 16);          // the types a client sends
            else if (pick < 7) type = uint8_t(100 + rng() % 20);   // the ones only the server sends
            else type = uint8_t(rng());                            // cualquiera
            auto p = type == uint8_t(Msg::Command) && rng() % 2 ? command(rng) : payload(rng);
            if (!r.sendRaw(type, p)) break;
            ++g_sent;
            if (!preauth && rng() % 3 == 0) {  // UDP: encrypted with odd content, or garbage
                std::vector<uint8_t> u = payload(rng);
                if (u.size() > 1400) u.resize(1400);
                if (!u.empty() && rng() % 2) u[0] = "VEKAP"[rng() % 5];
                if (rng() % 4) r.sendUdp(u);
                else r.sendUdpRaw(u);
                ++g_udp;
            }
            if (rng() % 50 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        r.close();
        drain.join();
        ++g_reconnects;
    }
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = uint16_t(argc > 1 ? std::atoi(argv[1]) : 19989);
    int seconds = argc > 2 ? std::atoi(argv[2]) : 30;
    Raw watch;
    if (!watch.connectTo("Watchdog", port)) { std::printf("FAIL: could not connect to the server\n"); return 2; }

    std::vector<std::thread> th;
    for (int i = 0; i < 12; ++i) th.emplace_back(fuzzer, i, port);

    // Watchdog: a stamped Ping every 0.5 s; the Pong has to arrive in less than 2 s
    std::atomic<uint64_t> pongs{0};
    std::thread rd([&] {
        std::vector<uint8_t> m;
        while (watch.readMsg(m))
            if (m[0] == uint8_t(Msg::Pong)) ++pongs;
    });
    int late = 0, pings = 0;
    auto end = clk::now() + std::chrono::seconds(seconds);
    while (clk::now() < end) {
        uint64_t before = pongs;
        Writer w;
        w.u64(uint64_t(pings));
        if (!watch.sendMsg(Msg::Ping, w.buf)) { std::printf("FAIL: the server closed the watchdog connection\n"); late = 999; break; }
        ++pings;
        auto t0 = clk::now();
        while (pongs == before && clk::now() - t0 < std::chrono::seconds(2)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (pongs == before) ++late;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    g_stop = true;
    for (auto& t : th) t.join();

    // Can people still log in?
    Raw after;
    bool alive = after.connectTo("Despues", port);
    watch.close();
    rd.join();
    std::printf("Sent: %llu TCP messages and %llu UDP packets in %d s, %llu connections\n",
                (unsigned long long)g_sent.load(), (unsigned long long)g_udp.load(), seconds,
                (unsigned long long)g_reconnects.load());
    std::printf("%s watchdog: %d of %d pings without a timely answer\n", late == 0 ? "ok   " : "FAIL ", late, pings);
    std::printf("%s logging in still works afterwards: %s\n", alive ? "ok   " : "FAIL ", alive ? "yes" : "no");
    return late == 0 && alive ? 0 : 1;
}
