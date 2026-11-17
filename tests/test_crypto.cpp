// Tests of the end-to-end encryption (Double Ratchet in private messages, signatures in channels).
// Each identity lives in its own temporary folder (APPDATA/HOME point to it when loading).
//   g++ -std=c++17 -O2 -Ithird_party/monocypher tests/test_crypto.cpp client/keys.cpp client/e2e.cpp
//       common/secure.cpp common/i18n.cpp third_party/monocypher/monocypher.c -o test_crypto
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <atomic>
#include <fstream>
#include <thread>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../client/keys.hpp"
#include "../common/invite.hpp"

using namespace eco;

// Any public key, to check that the server fingerprint gives 32 hex digits
static const uint8_t* A_dummy_id() {
    static uint8_t id[32] = {1, 2, 3};
    return id;
}
namespace fs = std::filesystem;

static int fails = 0, passes = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (c) ++passes;                                                 \
        else { ++fails; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static fs::path base;

static void setHome(const std::string& who) {
    fs::path d = base / who;
    fs::create_directories(d);
#ifdef _WIN32
    _putenv_s("APPDATA", d.string().c_str());
#else
    setenv("HOME", d.string().c_str(), 1);
#endif
}

static std::unique_ptr<Keys> open(const std::string& who) {
    setHome(who);
    auto k = std::make_unique<Keys>();
    std::string err;
    if (!k->load(err)) { std::printf("does not load %s: %s\n", who.c_str(), err.c_str()); std::exit(2); }
    return k;
}

static std::string adPm(const std::string& f, const std::string& t) { return "p|" + f + "|" + t; }

static bool recv(Keys& k, const std::string& sealed, const PubKey& from, const std::string& ad, const std::string& want) {
    std::string got;
    return k.openPm(sealed, from, ad, got) && got == want;
}

int main() {
    base = fs::temp_directory_path() / ("eco_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(base);
    std::printf("Test folder: %s\n", base.string().c_str());

    // ------------------------------------------------------------------ Double Ratchet
    {
        auto A = open("ana"), B = open("beto");
        PubKey a = A->pub(), b = B->pub();
        std::string ab = adPm("ana", "beto"), ba = adPm("beto", "ana");

        // 1) Round trip, several times
        auto m1 = A->sealPm("hola beto", b, ab);
        CHECK(m1.compare(0, 3, "\x01R:") == 0);
        CHECK(recv(*B, m1, a, ab, "hola beto"));
        auto r1 = B->sealPm("hola ana 😀", a, ba);
        CHECK(recv(*A, r1, b, ba, "hola ana 😀"));
        for (int i = 0; i < 20; ++i) {
            auto x = A->sealPm("a" + std::to_string(i), b, ab);
            CHECK(recv(*B, x, a, ab, "a" + std::to_string(i)));
            auto y = B->sealPm("b" + std::to_string(i), a, ba);
            CHECK(recv(*A, y, b, ba, "b" + std::to_string(i)));
        }

        // 2) Each message uses a different key: the same text gives different ciphertexts
        CHECK(A->sealPm("igual", b, ab) != A->sealPm("igual", b, ab));

        // 3) Out of order: 6 are sent and arrive 3,1,6,2,5,4
        std::vector<std::string> v;
        for (int i = 1; i <= 6; ++i) v.push_back(A->sealPm("m" + std::to_string(i), b, ab));
        // (the two earlier "same" ones are not delivered: they are lost; the skipped keys cover them)
        for (int i : {3, 1, 6, 2, 5, 4}) CHECK(recv(*B, v[i - 1], a, ab, "m" + std::to_string(i)));

        // 4) Tampered message: rejected and the state isn't damaged
        auto t1 = A->sealPm("intacto", b, ab);
        std::string bad = t1;
        bad[bad.size() / 2] = bad[bad.size() / 2] == 'A' ? 'B' : 'A';
        std::string tmp;
        CHECK(!B->openPm(bad, a, ab, tmp));
        CHECK(recv(*B, t1, a, ab, "intacto"));

        // 5) Associated data: if the server changes sender/destination, it doesn't open
        auto t2 = A->sealPm("para beto", b, ab);
        CHECK(!B->openPm(t2, a, adPm("eva", "beto"), tmp));
        CHECK(recv(*B, t2, a, ab, "para beto"));

        // 6) Another identity can't read it
        auto C = open("carla");
        CHECK(!C->openPm(t2, a, ab, tmp));

        // 7) Repeated (the server resends it): in the history it is read from the local copy, it doesn't break anything...
        CHECK(recv(*B, t2, a, ab, "para beto"));
        // ...but if it arrives "live" again, it is a replay and isn't shown as new
        {
            bool rep = false;
            CHECK(!B->openPm(t2, a, ab, tmp, true, &rep) && rep);
            auto fresh = A->sealPm("nuevo de verdad", b, ab);
            CHECK(B->openPm(fresh, a, ab, tmp, true, &rep) && !rep && tmp == "nuevo de verdad");
        }

        // 8) My own messages (server copy / history) are read from the local copy
        CHECK(recv(*A, t2, b, ab, "para beto"));

        // 9) Persistence: it is closed and reopened, and the conversation continues
        A.reset();
        B.reset();
        A = open("ana");
        B = open("beto");
        auto p1 = A->sealPm("tras reiniciar", b, ab);
        CHECK(recv(*B, p1, a, ab, "tras reiniciar"));
        auto p2 = B->sealPm("sigo aquí ñ", a, ba);
        CHECK(recv(*A, p2, b, ba, "sigo aquí ñ"));
        CHECK(recv(*B, t2, a, ab, "para beto"));  // the history is still readable after restarting

        // 10) Forward secrecy: without the local copy, a message already read can't be
        //     decrypted again even with the whole device state (its keys were deleted)
        B.reset();
        fs::remove(base / "beto" / "eco_privados.txt");
        fs::remove(base / "beto" / ".eco_privados");
        B = open("beto");
        CHECK(!B->openPm(p1, a, ab, tmp));
        auto p3 = A->sealPm("nuevo", b, ab);
        CHECK(recv(*B, p3, a, ab, "nuevo"));

        // 11) Many lost in a row (within the limit) and then one that does arrive
        std::string last;
        for (int i = 0; i < 300; ++i) last = A->sealPm("perdido " + std::to_string(i), b, ab);
        CHECK(recv(*B, last, a, ab, "perdido 299"));
    }

    // 12) Both start at the same time (before receiving anything from the other)
    {
        auto D = open("dani"), E = open("elena");
        PubKey d = D->pub(), e = E->pub();
        std::string de = adPm("dani", "elena"), ed = adPm("elena", "dani");
        auto x = D->sealPm("hola desde D", e, de);
        auto y = E->sealPm("hola desde E", d, ed);
        CHECK(recv(*E, x, d, de, "hola desde D"));
        CHECK(recv(*D, y, e, ed, "hola desde E"));
        for (int i = 0; i < 5; ++i) {
            auto p = D->sealPm("d" + std::to_string(i), e, de);
            auto q = E->sealPm("e" + std::to_string(i), d, ed);
            CHECK(recv(*E, p, d, de, "d" + std::to_string(i)));
            CHECK(recv(*D, q, e, ed, "e" + std::to_string(i)));
        }
    }

    // ------------------------------------------------------------------ Canales: firmas
    {
        auto A = open("ana"), B = open("beto"), C = open("carla");
        PubKey a = A->pub(), b = B->pub(), c = C->pub();
        A->rotate();
        C->rotate();
        // A hands its key to B and C; C its own to B
        auto wb = A->wrapFor(b), wc = A->wrapFor(c), cb = C->wrapFor(b);
        CHECK(wb.size() == 24 + 72 + 16);
        CHECK(B->unwrapFrom(a, wb.data(), wb.size()));
        CHECK(C->unwrapFrom(a, wc.data(), wc.size()));
        CHECK(B->unwrapFrom(c, cb.data(), cb.size()));
        // An envelope that claims to come from C but is from A: not accepted
        CHECK(!B->unwrapFrom(c, wb.data(), wb.size()));

        std::string ad = "c|ana|lobby", plain;
        auto s = A->sealText("mensaje firmado ✓ 🎉", ad);
        CHECK(s.compare(0, 3, "\x01S:") == 0);
        CHECK(B->openText(s, ad, &a, plain) && plain == "mensaje firmado ✓ 🎉");
        CHECK(!B->openText(s, ad, nullptr, plain));     // unknown sender and the key has an owner: no
        CHECK(B->openText(s, ad, &a, plain, true));     // live, the first time yes...
        CHECK(!B->openText(s, ad, &a, plain, true));    // ...repeated live: no
        CHECK(B->openText(s, ad, &a, plain));           // in the history yes
        CHECK(!B->openText(s, ad, &c, plain));          // the server says it's from C: doesn't match
        CHECK(A->openText(s, ad, &a, plain));           // the sender itself reads it
        CHECK(!B->openText(s, "c|ana|otro", &a, plain));  // movido a otro canal

        // Firma alterada
        std::vector<uint8_t> raw;
        b64decode(s.data() + 3, s.size() - 3, raw);
        auto bad = raw;
        bad.back() ^= 1;
        CHECK(!B->openText("\x01S:" + b64encode(bad), ad, &a, plain));
        // Unsigned and with the old marker (attempt to downgrade security): rejected
        std::vector<uint8_t> stripped(raw.begin(), raw.end() - 64);
        CHECK(!B->openText("\x01K:" + b64encode(stripped), ad, &a, plain));
        // C (who also knows A's symmetric key) tries to impersonate A: signs with
        // its own a message with A's key id -> the signature is not from A's key
        auto forged = C->sealText("soy ana, de verdad", ad);
        std::vector<uint8_t> fr;
        b64decode(forged.data() + 3, forged.size() - 3, fr);
        std::copy(raw.begin(), raw.begin() + 8, fr.begin());  // A's key id
        CHECK(!B->openText("\x01S:" + b64encode(fr), ad, &a, plain));

        // Pantalla firmada
        std::vector<uint8_t> frame(50000);
        for (size_t i = 0; i < frame.size(); ++i) frame[i] = uint8_t(i * 7);
        std::vector<uint8_t> sealed, out;
        uint8_t flags = 4 | 8;
        CHECK(A->sealBlob(frame.data(), frame.size(), flags, sealed));
        CHECK(B->openBlob(sealed.data(), sealed.size(), flags, true, &a, out) && out == frame);
        CHECK(!B->openBlob(sealed.data(), sealed.size(), flags, true, &c, out));
        CHECK(!B->openBlob(sealed.data(), sealed.size() - 64, 4, false, &a, out));  // unsigned: no
        sealed[100] ^= 1;
        CHECK(!B->openBlob(sealed.data(), sealed.size(), flags, true, &a, out));

        // Voice: the key has to belong to whoever is talking
        std::vector<uint8_t> opus(80, 0x55), sv, ov;
        CHECK(A->sealVoice(opus.data(), opus.size(), sv));
        CHECK(B->openVoice(sv.data(), sv.size(), &a, ov) && ov == opus);
        CHECK(A->sealVoice(opus.data(), opus.size(), sv));
        CHECK(!B->openVoice(sv.data(), sv.size(), &c, ov));
        // Replay of a voice packet: rejected
        CHECK(A->sealVoice(opus.data(), opus.size(), sv));
        CHECK(B->openVoice(sv.data(), sv.size(), &a, ov));
        CHECK(!B->openVoice(sv.data(), sv.size(), &a, ov));

        // Old key: A creates three more; what arrives live with the first one is a reinjection
        auto oldMsg = A->sealText("viejo", ad);
        for (int i = 0; i < 3; ++i) {
            A->rotate();
            auto wr = A->wrapFor(b);
            CHECK(B->unwrapFrom(a, wr.data(), wr.size()));
        }
        CHECK(!B->openText(oldMsg, ad, &a, plain, true));
        CHECK(B->openText(oldMsg, ad, &a, plain) && plain == "viejo");  // in the history yes
        CHECK(A->sealVoice(opus.data(), opus.size(), sv));
        CHECK(B->openVoice(sv.data(), sv.size(), &a, ov));  // with the new key, yes

        // After restarting, the keyring remembers owner and signing key
        B.reset();
        B = open("beto");
        CHECK(B->openText(s, ad, &a, plain));
        CHECK(!B->openText(s, ad, &c, plain));
        CHECK(!B->openText(s, ad, &a, plain, true));  // after restarting, no saved key is valid live

        // The keyring is encrypted on disk: no key appears in hexadecimal
        {
            fs::path rp = base / "beto" / "eco_llaves.txt";
            if (!fs::exists(rp)) rp = base / "beto" / ".eco_llaves";
            std::ifstream f(rp);
            std::string line;
            bool allV2 = true;
            int n = 0;
            while (std::getline(f, line)) { allV2 &= line.compare(0, 3, "v2 ") == 0; ++n; }
            CHECK(n > 0 && allV2);
        }

        // Verified: if the key of a verified one changes, it gets blocked
        B->setVerified("srv:1", "Ana", a);
        CHECK(B->isVerified("srv:1", "ana", a));
        CHECK(!B->blocked("srv:1", "ana", a));
        CHECK(B->blocked("srv:1", "ana", c));
        B->forgetVerified("srv:1", "ana");
        CHECK(!B->blocked("srv:1", "ana", c));
    }

    // ------------------------------------------------------------------ Invitaciones
    {
        const std::string fp = "7103-704b-5612-ab4e-faef-7063-269c-7cf6";
        Invite inv;
        auto s = makeInvite("mi-servidor.com", 9987, fp, "");
        CHECK(s == "eco://mi-servidor.com:9987/#f=7103704b5612ab4efaef7063269c7cf6");
        CHECK(parseInvite(s, inv) && inv.host == "mi-servidor.com" && inv.port == 9987 &&
              inv.fingerprint == "7103704b5612ab4efaef7063269c7cf6" && inv.serverPass.empty());
        // Password with odd characters, IPv6 and spaces around (when pasting)
        s = makeInvite("2001:db8::1", 4000, fp, "clave & ñ #=%");
        CHECK(parseInvite("  " + s + " \n", inv) && inv.host == "2001:db8::1" && inv.port == 4000 &&
              inv.serverPass == "clave & ñ #=%");
        CHECK(parseInvite("eco://1.2.3.4/#f=" + normalizeFingerprint(fp), inv) && inv.port == kDefaultPort);
        CHECK(!parseInvite("eco://1.2.3.4:9987/", inv));                 // without a fingerprint it isn't valid
        CHECK(!parseInvite("eco://1.2.3.4:9987/#f=1234", inv));          // huella corta
        CHECK(!parseInvite("eco://1.2.3.4:99999/#f=" + normalizeFingerprint(fp), inv));  // puerto imposible
        CHECK(!parseInvite("eco://a b/#f=" + normalizeFingerprint(fp), inv));            // host with a space
        CHECK(!parseInvite("http://1.2.3.4/#f=" + normalizeFingerprint(fp), inv));
        CHECK(normalizeFingerprint(fingerprint(A_dummy_id())) .size() == 32);
    }

    // ------------------------------------------------------------------ Rendimiento
    {
        auto A = open("ana"), B = open("beto");
        PubKey a = A->pub(), b = B->pub();
        std::string ab = adPm("ana", "beto"), ba = adPm("beto", "ana");
        auto t0 = std::chrono::steady_clock::now();
        const int N = 200;
        for (int i = 0; i < N; ++i) {  // alternating: each message is a full DH step
            auto x = A->sealPm("x", b, ab);
            std::string p;
            B->openPm(x, a, ab, p);
            auto y = B->sealPm("y", a, ba);
            A->openPm(y, b, ba, p);
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("Double Ratchet: %.3f ms per message (encrypt + decrypt + save state)\n", ms / (2 * N));

        A->rotate();
        auto w = A->wrapFor(b);
        B->unwrapFrom(a, w.data(), w.size());
        t0 = std::chrono::steady_clock::now();
        std::string p;
        for (int i = 0; i < N; ++i) B->openText(A->sealText("mensaje de canal", "c|a|l"), "c|a|l", &a, p);
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("Signed channel: %.3f ms per message (sign + verify + encrypt + decrypt)\n", ms / N);

        // Audio capture (sealVoice every 20 ms) must not wait for private messages, which save to disk
        std::atomic<bool> busy{true};
        std::thread pm([&] {
            while (busy) {
                auto x = A->sealPm("privado mientras hablo", b, ab);
                std::string q;
                B->openPm(x, a, ab, q);
            }
        });
        std::vector<uint8_t> op(80, 1), sv;
        double worst = 0;
        for (int i = 0; i < 500; ++i) {
            auto v0 = std::chrono::steady_clock::now();
            A->sealVoice(op.data(), op.size(), sv);
            worst = std::max(worst, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - v0).count());
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        busy = false;
        pm.join();
        std::printf("Voice while private messages are sent: worst case %.0f us per packet (it used to wait for the disk)\n", worst);
        CHECK(worst < 5000);  // far below the 20 ms of an audio frame
    }

    std::error_code ec;
    fs::remove_all(base, ec);
    std::printf("\n%d checks passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
