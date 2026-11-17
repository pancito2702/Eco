// A malicious server against the client: does the real greeting (with its own identity) and then sends
// the client thousands of encrypted messages with random or malformed content, of every type, over TCP and over
// UDP. The client has to stay alive and connected.
//   eco-test-fuzz-client 19990 30        and in another console:     eco-client 127.0.0.1:19990 Test --trust --tone
#include <asio.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "../common/protocol.hpp"
#include "../common/secure.hpp"

using namespace eco;
using asio::ip::tcp;
using asio::ip::udp;
using clk = std::chrono::steady_clock;

namespace {

std::vector<uint8_t> randomBytes(std::mt19937& rng, size_t max) {
    std::vector<uint8_t> v(rng() % (max + 1));
    for (auto& b : v) b = uint8_t(rng());
    return v;
}

std::string randomStr(std::mt19937& rng) {
    static const char* bits[] = {"Ana", "\xff", "\xe2\x80\xae", "\x1b[2J", "\r\n", "Lobby", "\x01", "%n%s", "Ádmin", "\xf0\x9f"};
    std::string s;
    for (int i = 0, n = int(rng() % 6); i < n; ++i) s += bits[rng() % 10];
    return s;
}

// Server messages with "shape" (to get deeper into the client than with random bytes)
std::vector<uint8_t> shaped(std::mt19937& rng, Msg& type) {
    Writer w;
    switch (rng() % 12) {
        case 0: {  // State with made-up users (small ids: that way they later match ScreenFrom, KeyFrom...)
            type = Msg::State;
            int nc = int(rng() % 4);
            w.u16(uint16_t(nc));
            for (int c = 0; c < nc; ++c) {
                w.str(c ? randomStr(rng) : "Lobby");
                int nu = int(rng() % 5);
                w.u16(uint16_t(nu));
                for (int u = 0; u < nu; ++u) {
                    w.u32(uint32_t(rng() % 10));
                    w.str(randomStr(rng) + std::to_string(u));
                    w.u8(uint8_t(rng() % 4));
                    w.u8(uint8_t(rng()));
                    auto k = randomBytes(rng, 0);
                    for (int i = 0; i < 32; ++i) w.u8(uint8_t(rng()));
                }
            }
            break;
        }
        case 1:
            type = Msg::ChatMsg;
            w.str(randomStr(rng));
            w.str(randomStr(rng));
            w.str(std::string(1, '\x01') + randomStr(rng));
            w.u8(uint8_t(rng() % 4));
            break;
        case 2: {
            type = Msg::History;
            w.str(randomStr(rng));
            w.str("Lobby");
            int n = int(rng() % 300);
            w.u16(uint16_t(rng() % 3 == 0 ? 65535 : n));
            for (int i = 0; i < n; ++i) { w.u64(rng()); w.str(randomStr(rng)); w.str(randomStr(rng)); w.str("\x01" + randomStr(rng)); }
            break;
        }
        case 3: {
            type = Msg::ScreenFrom;
            w.u32(uint32_t(rng() % 10));
            w.u8(uint8_t(rng()));
            auto b = randomBytes(rng, 3000);
            w.raw(b.data(), b.size());
            break;
        }
        case 4: {
            type = Msg::KeyFrom;
            w.u32(uint32_t(rng() % 10));
            auto b = randomBytes(rng, 300);
            w.bytes(b.data(), b.size());
            break;
        }
        case 5: {
            type = Msg::PeerKey;
            w.str(randomStr(rng));
            w.u8(uint8_t(rng() % 2));
            for (int i = 0; i < 32; ++i) w.u8(uint8_t(rng()));
            break;
        }
        case 6: {
            type = Msg::UpdateInfo;
            w.str(rng() % 2 ? "windows-x64" : randomStr(rng));
            std::string man = "eco-actualizacion 1\nversion 99.0.0\nplataforma windows-x64\narchivo eco-client.exe 10 " +
                              std::string(64, 'a') + "\n";
            w.bytes(reinterpret_cast<const uint8_t*>(man.data()), man.size());
            auto sig = randomBytes(rng, 70);
            w.bytes(sig.data(), sig.size());
            break;
        }
        case 7: {
            type = Msg::UpdateChunk;
            w.str("eco-client.exe");
            w.u32(uint32_t(rng()));
            w.u32(uint32_t(rng()));
            auto b = randomBytes(rng, 500);
            w.bytes(b.data(), b.size());
            break;
        }
        case 8: {  // voice or screen audio over TCP
            type = Msg::VoiceFrom;
            w.u8(uint8_t("KEAVP"[rng() % 5]));
            w.u32(uint32_t(rng() % 10));
            auto b = randomBytes(rng, 1300);
            w.raw(b.data(), b.size());
            break;
        }
        case 9:
            type = Msg::Viewers;
            w.u16(uint16_t(rng()));
            break;
        case 10:
            type = Msg::AuthFail;
            w.str(randomStr(rng));
            break;
        default: {  // any server type except Kicked (that one disconnects, which is what it should do)
            int t;
            do t = 100 + int(rng() % 20); while (t == int(Msg::Kicked));
            type = Msg(t);
            auto b = randomBytes(rng, 400);
            w.raw(b.data(), b.size());
        }
    }
    return w.buf;
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = uint16_t(argc > 1 ? std::atoi(argv[1]) : 19990);
    int seconds = argc > 2 ? std::atoi(argv[2]) : 30;
    asio::io_context io;
    tcp::acceptor acc(io, tcp::endpoint(tcp::v4(), port));
    udp::socket us(io, udp::endpoint(udp::v4(), port));
    std::printf("Malicious server waiting on port %u...\n", unsigned(port));
    std::fflush(stdout);
    tcp::socket s(io);
    acc.accept(s);

    // Real greeting, signed with its own identity (the client has to have accepted it with --trust)
    uint8_t hello[4 + kClientHelloSize];
    asio::read(s, asio::buffer(hello));
    const uint8_t* cpub = hello + 12;
    uint32_t version = uint32_t(hello[8]) << 24 | uint32_t(hello[9]) << 16 | uint32_t(hello[10]) << 8 | hello[11];
    uint8_t seed[32], idSecret[64], idPub[32], esk[32], spub[32], shared[32];
    secureRandom(seed, 32);
    crypto_eddsa_key_pair(idSecret, idPub, seed);
    secureRandom(esk, 32);
    crypto_x25519_public_key(spub, esk);
    crypto_x25519(shared, esk, cpub);
    SessionKeys keys;
    deriveKeys(shared, cpub, spub, idPub, keys);
    auto tr = handshakeTranscript(version, cpub, spub);
    std::vector<uint8_t> rep(4 + kServerHelloSize);
    rep[3] = uint8_t(kServerHelloSize);
    std::memcpy(rep.data() + 4, spub, 32);
    std::memcpy(rep.data() + 36, idPub, 32);
    crypto_eddsa_sign(rep.data() + 68, idSecret, tr.data(), tr.size());
    asio::write(s, asio::buffer(rep));
    TcpCipher tx, rx;
    tx.key = keys.tcpS2C;
    rx.key = keys.tcpC2S;
    auto send = [&](Msg t, const std::vector<uint8_t>& p) {
        auto b = body(t, p);
        auto out = tx.seal(b.data(), b.size());
        std::error_code ec;
        asio::write(s, asio::buffer(out), ec);
        return !ec;
    };

    // Whatever the client sends is read and discarded; if it closes the connection, it is noticed here
    std::atomic<bool> closed{false};
    std::thread rd([&] {
        for (;;) {
            uint8_t hd[4];
            std::error_code ec;
            asio::read(s, asio::buffer(hd), ec);
            if (ec) break;
            uint32_t n = uint32_t(hd[0]) << 24 | uint32_t(hd[1]) << 16 | uint32_t(hd[2]) << 8 | hd[3];
            std::vector<uint8_t> b(n);
            asio::read(s, asio::buffer(b), ec);
            if (ec || !rx.open(b.data(), n)) break;
        }
        closed = true;
    });

    Writer wel;
    wel.u32(5);
    wel.u16(port);
    wel.u8(0);
    wel.u8(0);
    wel.u8(0xFF);  // claims to have all the features
    wel.str("99.0.0");
    send(Msg::Welcome, wel.buf);

    // UDP: the client's address is learned from its first ping and odd encrypted packets are sent to it
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> udpSent{0};
    std::thread ut([&] {
        us.non_blocking(true);
        udp::endpoint client;
        bool known = false;
        std::vector<uint8_t> buf(2048), seal;
        std::mt19937 rng(7);
        uint64_t ctr = 0;
        while (!stop) {
            std::error_code ec;
            udp::endpoint from;
            us.receive_from(asio::buffer(buf), from, 0, ec);
            if (!ec) { client = from; known = true; }
            if (!known) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
            for (int i = 0; i < 20; ++i) {
                std::vector<uint8_t> p;
                p.push_back(uint8_t("KEAVPX"[rng() % 6]));
                auto r = randomBytes(rng, 1300);
                p.insert(p.end(), r.begin(), r.end());
                if (p.size() > 5 && rng() % 2) p[4] = uint8_t(rng() % 10);  // an id that exists sometimes
                udpSealInto(seal, keys.udpS2C, rng() % 8 == 0 ? ctr - 3 : ctr++, nullptr, 0, p.data(), p.size());
                us.send_to(asio::buffer(seal), client, 0, ec);
                ++udpSent;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    std::mt19937 rng(42);
    uint64_t sent = 0;
    auto end = clk::now() + std::chrono::seconds(seconds);
    while (clk::now() < end && !closed) {
        Msg t;
        auto p = rng() % 4 ? shaped(rng, t) : (t = Msg(100 + rng() % 20), randomBytes(rng, 2000));
        if (t == Msg::Kicked) continue;
        if (!send(t, p)) break;
        ++sent;
        if (sent % 200 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    bool ok = !closed;
    stop = true;
    ut.join();
    std::printf("Sent to the client: %llu TCP messages and %llu UDP packets in %d s\n", (unsigned long long)sent,
                (unsigned long long)udpSent.load(), seconds);
    std::printf("%s the client is still connected at the end\n", ok ? "ok   " : "FAIL ");
    std::error_code ec;
    s.shutdown(tcp::socket::shutdown_both, ec);
    s.close(ec);
    rd.join();
    return ok ? 0 : 1;
}
