// "Raw" client for the tests: speaks Eco's protocol directly (greeting, TCP and UDP encryption) to
// be able to send the server things a normal client would never send.
#pragma once
#include <asio.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../common/protocol.hpp"
#include "../common/secure.hpp"

namespace eco::test {

using asio::ip::tcp;
using asio::ip::udp;
using clk = std::chrono::steady_clock;

struct Raw {
    asio::io_context io;
    tcp::socket t{io};
    udp::socket u{io};
    udp::endpoint srv;
    SessionKeys k;
    TcpCipher tx, rx;
    uint32_t id = 0;
    uint64_t udpCtr = 0;

    // Only the encrypted greeting (without identifying): to test what arrives before Hello
    // from: local outgoing address (127.x.y.z: each one counts as a different IP for the server's brakes)
    bool handshake(uint16_t port, const std::string& from = "") {
        std::error_code ec;
        if (!from.empty()) {
            t.open(tcp::v4(), ec);
            t.bind(tcp::endpoint(asio::ip::make_address(from), 0), ec);
            if (ec) return false;
        }
        t.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        if (ec) return false;
        t.set_option(tcp::no_delay(true));
        uint8_t esk[32], cpub[32];
        secureRandom(esk, 32);
        crypto_x25519_public_key(cpub, esk);
        std::vector<uint8_t> h = {0, 0, 0, 40, 'E', 'C', 'O', '2'};
        for (int s = 24; s >= 0; s -= 8) h.push_back(uint8_t(kProtoVersion >> s));
        h.insert(h.end(), cpub, cpub + 32);
        asio::write(t, asio::buffer(h), ec);
        uint8_t rep[4 + kServerHelloSize];
        asio::read(t, asio::buffer(rep), ec);
        if (ec) return false;
        uint8_t shared[32];
        crypto_x25519(shared, esk, rep + 4);
        deriveKeys(shared, cpub, rep + 4, rep + 36, k);
        tx.key = k.tcpC2S;
        rx.key = k.tcpS2C;
        return true;
    }

    bool connectTo(const std::string& nick, uint16_t port, const std::string& from = "") {
        if (!handshake(port, from)) return false;
        Writer w;
        w.str(nick);
        w.str("");
        w.str("");
        uint8_t pk[32] = {9};
        w.raw(pk, 32);
        sendMsg(Msg::Hello, w.buf);
        std::vector<uint8_t> b;
        while (readMsg(b))
            if (b[0] == uint8_t(Msg::Welcome)) {
                Reader r(b.data() + 1, b.size() - 1);
                id = r.u32();
                srv = udp::endpoint(asio::ip::make_address("127.0.0.1"), r.u16());
                u.open(udp::v4());
                u.bind(udp::endpoint(udp::v4(), 0));
                u.set_option(asio::socket_base::receive_buffer_size(4 << 20));
                sendUdp({kUdpPing});
                return true;
            }
        return false;
    }
    bool sendMsg(Msg m, const std::vector<uint8_t>& p) { return sendRaw(uint8_t(m), p); }
    bool sendRaw(uint8_t type, const std::vector<uint8_t>& p) {
        std::vector<uint8_t> b;
        b.reserve(1 + p.size());
        b.push_back(type);
        b.insert(b.end(), p.begin(), p.end());
        auto out = tx.seal(b.data(), b.size());
        std::error_code ec;
        asio::write(t, asio::buffer(out), ec);
        return !ec;
    }
    bool readMsg(std::vector<uint8_t>& b) {
        uint8_t hd[4];
        std::error_code ec;
        asio::read(t, asio::buffer(hd), ec);
        if (ec) return false;
        uint32_t n = uint32_t(hd[0]) << 24 | uint32_t(hd[1]) << 16 | uint32_t(hd[2]) << 8 | hd[3];
        if (n < kMacSize + 1 || n > kMaxFrame) return false;
        b.resize(n);
        asio::read(t, asio::buffer(b), ec);
        if (ec || !rx.open(b.data(), n)) return false;
        b.resize(n - kMacSize);
        return true;
    }
    void sendUdp(const std::vector<uint8_t>& plain) {
        uint8_t pre[4] = {uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)};
        auto pkt = udpSeal(k.udpC2S, udpCtr++, pre, 4, plain.data(), plain.size());
        std::error_code ec;
        u.send_to(asio::buffer(pkt), srv, 0, ec);
    }
    void sendUdpRaw(const std::vector<uint8_t>& bytes) {  // unencrypted: garbage
        std::error_code ec;
        u.send_to(asio::buffer(bytes), srv, 0, ec);
    }
    // Counts UDP packets of one type for ms milliseconds
    int countUdp(uint8_t kind, int ms) {
        u.non_blocking(true);
        int n = 0;
        auto end = clk::now() + std::chrono::milliseconds(ms);
        std::vector<uint8_t> buf(2048), plain;
        udp::endpoint from;
        while (clk::now() < end) {
            std::error_code ec;
            size_t r = u.receive_from(asio::buffer(buf), from, 0, ec);
            if (ec) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            uint64_t c;
            if (udpOpen(k.udpS2C, buf.data(), r, 0, c, plain) && plain[0] == kind) ++n;
        }
        return n;
    }
    // Counts TCP messages of one type for ms milliseconds (read on another thread)
    int countTcp(Msg type, int ms) {
        int n = 0;
        std::thread rd([&] {
            std::vector<uint8_t> m;
            while (readMsg(m))
                if (m[0] == uint8_t(type)) ++n;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        std::error_code ec;
        t.shutdown(tcp::socket::shutdown_both, ec);
        rd.join();
        return n;
    }
    void close() {
        std::error_code ec;
        t.close(ec);
        u.close(ec);
    }
};

}  // namespace eco::test
