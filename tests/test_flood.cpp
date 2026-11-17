// Test of the server's flood brakes. Needs an eco-server running locally:
//   eco-server --port 19988 --db test.db --backups 0      and then      eco-test-flood 19988
// Two "raw" clients that speak the protocol directly: one floods and the other counts what reaches it.
// Exits with a non-zero code if any brake doesn't work.
#include <cstdio>
#include <cstdlib>

#include "raw_client.hpp"

using namespace eco;
using eco::test::Raw;

int main(int argc, char** argv) {
    uint16_t port = uint16_t(argc > 1 ? std::atoi(argv[1]) : 19988);
    Raw a, b;
    if (!a.connectTo("Inunda", port) || !b.connectTo("Cuenta", port)) {
        std::printf("FAIL: could not connect to the server on port %u\n", unsigned(port));
        return 2;
    }
    int fails = 0;
    auto check = [&](bool ok, const char* what, int got, const char* expect) {
        std::printf("%s %s: %d (expected %s)\n", ok ? "ok   " : "FAIL ", what, got, expect);
        fails += !ok;
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    b.countUdp(0, 200);  // drains the pongs

    // 1) Voice over UDP: 3,000 packets in a row
    std::thread t1([&] {
        for (int i = 0; i < 3000; ++i) {
            std::vector<uint8_t> p = {kUdpVoiceAuto, uint8_t(i >> 8), uint8_t(i)};
            p.resize(200, 0x55);
            a.sendUdp(p);
        }
    });
    int got = b.countUdp(kUdpVoiceAuto, 2000);
    t1.join();
    check(got >= 250 && got <= 700, "UDP voice relayed out of 3000 in a row", got, "~300: burst + rate");

    // 2) Screen audio: only to whoever watches and only with the flag
    a.sendMsg(Msg::ScreenStart, {});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Writer w;
    w.u32(a.id);
    b.sendMsg(Msg::Watch, w.buf);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // fichas UDP recuperadas
    std::thread t2([&] {
        for (int i = 0; i < 40; ++i) {
            std::vector<uint8_t> p = {kUdpScreenAudio, kScreenAudio | kScreenAuto};
            p.resize(500, 0x33);
            a.sendUdp(p);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::vector<uint8_t> bad = {kUdpScreenAudio, kScreenAuto};  // without the audio flag: not relayed
        bad.resize(100, 1);
        for (int i = 0; i < 10; ++i) a.sendUdp(bad);
    });
    got = b.countUdp(kUdpScreenAudio, 1500);
    t2.join();
    check(got == 40, "screen audio received (40 valid + 10 without flag)", got, "40");

    // 3) Thousands of tiny frames over TCP
    std::thread t3([&] {
        for (int i = 0; i < 5000; ++i) a.sendMsg(Msg::ScreenFrame, {kScreenKey | kScreenAuto, 1, 2, 3});
    });
    int frames = b.countTcp(Msg::ScreenFrom, 2000);
    t3.join();
    check(frames >= 400 && frames <= 1300, "tiny frames relayed out of 5000", frames, "~520: burst + rate");

    std::printf(fails ? "\n%d checks failed\n" : "\nThe brakes work\n", fails);
    return fails ? 1 : 0;
}
