// eco-sign: publishes a new version of Eco so clients update by themselves.
//
//   eco-sign new-key <archivo-clave>
//       Creates your signing key (keep it safe and NEVER hand it out) and, next to it, update_public_key.txt with the
//       public key. CMake puts it in the clients if it is in local_data_DO_NOT_DISTRIBUTE/ (or with
//       -DECO_UPDATE_PUBLIC_KEY=...): the clients you build will only accept versions signed with your key.
//   eco-sign publish <key-file> <version> <platform> <updates-folder> <file>...
//       Copies the files to <folder>/<platform>/ and writes manifest.txt and manifest.sig there.
//       The server hands it out with: eco-server --updates <folder>
//   eco-sign verify <updates-folder> <platform>
//       Checks the signature and the files with the public key built into this program.
// (The old names also work: nueva-clave, publicar, comprobar.)
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../common/secure.hpp"
#include "../common/update.hpp"
#include "../common/update_key.hpp"
#include "../common/version.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace eco;
namespace fs = std::filesystem;

static bool readAll(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static bool loadKey(const std::string& file, uint8_t secret[64], uint8_t pub[32]) {
    std::ifstream f(file);
    std::string h;
    f >> h;
    uint8_t seed[32];
    if (!update::unhex(h, seed, 32)) return false;
    crypto_eddsa_key_pair(secret, pub, seed);  // wipes the seed
    return true;
}

static int usage() {
    std::cout << "eco-sign " << kVersion << "\n"
                 "  eco-sign new-key <key-file>\n"
                 "  eco-sign publish <key-file> <version> <platform> <folder> <file>...\n"
                 "  eco-sign verify <folder> <platform>\n";
    return 1;
}

int main(int argc, char** argv) {
    std::vector<std::string> a(argv + 1, argv + argc);
    if (a.empty()) return usage();
    if (a[0] == "nueva-clave") a[0] = "new-key";
    if (a[0] == "publicar") a[0] = "publish";
    if (a[0] == "comprobar") a[0] = "verify";
    if (a[0] == "new-key" && a.size() == 2) {
        if (fs::exists(a[1])) { std::cerr << a[1] << " already exists: not overwriting it\n"; return 1; }
        uint8_t seed[32], secret[64], pub[32];
        secureRandom(seed, 32);
        std::string h = update::hex(seed, 32);
        crypto_eddsa_key_pair(secret, pub, seed);
        {
            std::ofstream f(a[1]);
            f << h << "\n";
        }
#ifndef _WIN32
        ::chmod(a[1].c_str(), 0600);
#endif
        crypto_wipe(secret, 64);
        fs::path pubFile = fs::path(a[1]).parent_path() / "update_public_key.txt";
        std::ofstream(pubFile) << update::hex(pub, 32) << "\n";
        std::cout << "Key saved in " << a[1] << " (don't hand it out: whoever has it can publish versions).\n"
                  << "Public key (in " << pubFile.string() << "):\n" << update::hex(pub, 32) << "\n"
                  << "For your clients to accept it, build them with it: leave it in local_data_DO_NOT_DISTRIBUTE/ "
                     "or use -DECO_UPDATE_PUBLIC_KEY=<key>.\n";
        return 0;
    }
    if (a[0] == "publish" && a.size() >= 6) {
        uint8_t secret[64], pub[32];
        if (!loadKey(a[1], secret, pub)) { std::cerr << "Could not read the key " << a[1] << "\n"; return 1; }
        if (std::memcmp(pub, kUpdateKey, 32) != 0)
            std::cout << "WARNING: this key is not the one the clients built now carry (update_public_key.txt): "
                         "they won't accept this version.\n";
        const std::string& version = a[2];
        const std::string& plat = a[3];
        fs::path dir = fs::path(a[4]) / plat;
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string man = "eco-actualizacion 1\nversion " + version + "\nplataforma " + plat + "\n";
        for (size_t i = 5; i < a.size(); ++i) {
            fs::path src(a[i]);
            std::string name = src.filename().string();
            std::vector<uint8_t> data;
            if (!update::safeName(name) || !readAll(src, data) || data.empty()) {
                std::cerr << "Could not read " << a[i] << "\n";
                return 1;
            }
            uint8_t h[32];
            crypto_blake2b(h, 32, data.data(), data.size());
            // Deleted first instead of using overwrite_existing: on MinGW that option fails if the file already exists
            if (!fs::equivalent(src, dir / name, ec)) {
                ec.clear();
                fs::remove(dir / name, ec);
                ec.clear();
                fs::copy_file(src, dir / name, ec);
            }
            if (ec) { std::cerr << "Could not copy " << a[i] << ": " << ec.message() << "\n"; return 1; }
            man += "archivo " + name + " " + std::to_string(data.size()) + " " + update::hex(h, 32) + "\n";
        }
        uint8_t sig[64];
        update::sign(man, secret, sig);
        crypto_wipe(secret, 64);
        std::ofstream(dir / "manifest.txt", std::ios::binary) << man;
        std::ofstream(dir / "manifest.sig", std::ios::binary).write(reinterpret_cast<const char*>(sig), 64);
        fs::remove(dir / "manifiesto.txt", ec);  // the names from before 2.3
        fs::remove(dir / "manifiesto.sig", ec);
        std::cout << "Published version " << version << " for " << plat << " in " << dir.string() << ":\n" << man;
        return 0;
    }
    if (a[0] == "verify" && a.size() == 3) {
        fs::path dir = fs::path(a[1]) / a[2];
        std::vector<uint8_t> man, sig;
        if (!readAll(dir / "manifest.txt", man) || !readAll(dir / "manifest.sig", sig) || sig.size() != 64) {
            std::cerr << "Missing manifest or its signature in " << dir.string() << "\n";
            return 1;
        }
        std::string text(man.begin(), man.end());
        update::Manifest m;
        if (!update::verify(text, sig.data(), kUpdateKey)) { std::cerr << "INVALID SIGNATURE\n"; return 1; }
        if (!update::parse(text, m)) { std::cerr << "Manifiesto mal formado\n"; return 1; }
        bool ok = true;
        for (auto& f : m.files) {
            std::vector<uint8_t> data;
            uint8_t h[32];
            bool good = readAll(dir / f.name, data) && data.size() == f.size;
            if (good) { crypto_blake2b(h, 32, data.data(), data.size()); good = std::memcmp(h, f.hash, 32) == 0; }
            std::cout << (good ? "  bien  " : "  MAL   ") << f.name << "\n";
            ok &= good;
        }
        std::cout << (ok ? "Signature and files correct: version " : "Some files don't match: version ") << m.version << "\n";
        return ok ? 0 : 1;
    }
    return usage();
}
