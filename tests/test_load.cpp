// Server load: N clients in the same channel talking at once (20 ms voice packets, 50 per second
// each). The server has to relay each packet to the other N-1. Measures how many arrive.
//   eco-server --port 19987 --backups 0      and then      eco-test-load 19987 [clients] [seconds]
// The server's CPU usage is watched from outside (build.ps1 or Task Manager).
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "raw_client.hpp"

using namespace eco;
using eco::test::Raw;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv) {
    uint16_t port = uint16_t(argc > 1 ? std::atoi(argv[1]) : 19987);
    int n = argc > 2 ? std::atoi(argv[2]) : 30;
    int seconds = argc > 3 ? std::atoi(argv[3]) : 10;
    std::vector<std::unique_ptr<Raw>> cs;
    for (int i = 0; i < n; ++i) {
        auto r = std::make_unique<Raw>();
        // Each one from its own loopback IP (the server limits connections per IP)
        std::string from = "127.0." + std::to_string(1 + i / 200) + "." + std::to_string(1 + i % 200);
        if (!r->connectTo("load" + std::to_string(i), port, from)) { std::printf("FAIL connecting number %d\n", i); return 2; }
        cs.push_back(std::move(r));
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> got{0};
    std::vector<std::thread> th;
    // Receiving: all UDP sockets on one thread
    th.emplace_back([&] {
        std::vector<uint8_t> buf(2048);
        for (auto& c : cs) c->u.non_blocking(true);
        while (!stop) {
            bool any = false;
            for (auto& c : cs) {
                std::error_code ec;
                asio::ip::udp::endpoint from;
                while (c->u.receive_from(asio::buffer(buf), from, 0, ec), !ec) { ++got; any = true; }
            }
            if (!any) std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });
    // Sending: 50 packets per second per client
    uint64_t sent = 0;
    auto t0 = clk::now(), next = t0;
    std::vector<uint8_t> p(1 + 2 + 120, 0x5a);
    p[0] = kUdpVoiceAuto;
    while (clk::now() - t0 < std::chrono::seconds(seconds)) {
        next += std::chrono::milliseconds(20);
        for (auto& c : cs) { c->sendUdp(p); ++sent; }
        std::this_thread::sleep_until(next);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop = true;
    for (auto& t : th) t.join();
    double expected = double(sent) * (n - 1);
    std::printf("%d clients, %d s: sent %llu, expected relays %.0f, received %llu (%.1f %%), %.0f relays/s\n", n,
                seconds, (unsigned long long)sent, expected, (unsigned long long)got.load(), 100.0 * double(got) / expected,
                double(got) / seconds);
    return 0;
}
