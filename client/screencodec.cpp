#include "screencodec.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "../common/protocol.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS  // without the global text of the last error: decoding happens from several threads at once
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace eco {

namespace {
// Splits "n" jobs between this thread and a few more (tiles are independent). With few tiles
// (the usual case: one window changes) it isn't worth launching threads.
template <class F>
void parallelFor(size_t n, F work) {
    unsigned hw = std::thread::hardware_concurrency();
    size_t extra = n >= 12 ? std::min<size_t>(hw > 1 ? hw - 1 : 0, 3) : 0;
    std::atomic<size_t> next{0};
    auto run = [&] {
        for (size_t i; (i = next++) < n;) work(i);
    };
    std::vector<std::thread> th;
    for (size_t k = 0; k < extra; ++k) th.emplace_back(run);
    run();
    for (auto& t : th) t.join();
}
}  // namespace

void downscale(const Image& src, int maxWidth, Image& dst) {
    if (src.w <= maxWidth) { dst = src; return; }
    int dw = maxWidth;
    int dh = std::max(1, int(int64_t(src.h) * dw / src.w));
    dst.w = dw;
    dst.h = dh;
    dst.bgra.resize(size_t(dw) * dh * 4);
    // The limits of each column are computed once, not on every pixel of every row
    std::vector<int> xs(size_t(dw) + 1);
    for (int x = 0; x <= dw; ++x) xs[size_t(x)] = int(int64_t(x) * src.w / dw);
    const uint8_t* sp = src.bgra.data();
    const size_t stride = size_t(src.w) * 4;
    // Area average (good quality for text, linear cost). In bands of 16 rows, in parallel
    constexpr int kBand = 16;
    parallelFor(size_t((dh + kBand - 1) / kBand), [&](size_t band) {
      int yEnd = std::min(dh, int(band + 1) * kBand);
      for (int y = int(band) * kBand; y < yEnd; ++y) {
        int y0 = int(int64_t(y) * src.h / dh), y1 = std::max(y0 + 1, int(int64_t(y + 1) * src.h / dh));
        const uint8_t* rows = sp + size_t(y0) * stride;
        uint8_t* d = dst.bgra.data() + size_t(y) * dw * 4;
        for (int x = 0; x < dw; ++x, d += 4) {
            int x0 = xs[size_t(x)], x1 = std::max(x0 + 1, xs[size_t(x) + 1]);
            uint32_t b = 0, g = 0, r = 0;
            const uint8_t* row = rows + size_t(x0) * 4;
            for (int sy = y0; sy < y1; ++sy, row += stride) {
                const uint8_t* p = row;
                for (int sx = x0; sx < x1; ++sx, p += 4) { b += p[0]; g += p[1]; r += p[2]; }
            }
            uint32_t n = uint32_t(x1 - x0) * uint32_t(y1 - y0);
            d[0] = uint8_t(b / n); d[1] = uint8_t(g / n); d[2] = uint8_t(r / n); d[3] = 255;
        }
      }
    });
}

static void appendJpeg(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}

// Fast digest of the capture (four 64-bit lanes, several GB/s). It only serves to know whether the
// screen is identical to the previous one; it isn't cryptographic and never leaves this computer.
static uint64_t frameHash(const uint8_t* p, size_t n) {
    const uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t h[4] = {0x243F6A8885A308D3ull, 0x13198A2E03707344ull, 0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ull};
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        std::memcpy(w, p + i, 32);
        for (int j = 0; j < 4; ++j) {
            h[j] = (h[j] ^ w[j]) * k;
            h[j] ^= h[j] >> 29;
        }
    }
    uint64_t t = n;
    for (; i < n; ++i) t = (t ^ p[i]) * k;
    for (int j = 0; j < 4; ++j) {
        t = (t ^ h[j]) * k;
        t ^= t >> 32;
    }
    return t;
}

bool ScreenEncoder::encode(const Image& screen, bool forceKey, std::vector<uint8_t>& payload) {
    // If the image has to be scaled down, first check whether the screen is identical to the previous one (the usual
    // case almost always: nobody moves anything): that saves scaling and comparing it. Without scaling it is compared
    // directly with the previous one, without copying it first.
    const bool scale = screen.w > maxWidth;
    if (scale) {
        uint64_t h = frameHash(screen.bgra.data(), screen.bgra.size());
        bool same = hashed_ && h == lastHash_ && screen.w == lastW_ && screen.h == lastH_ && maxWidth == lastMax_;
        hashed_ = true;
        lastHash_ = h;
        lastW_ = screen.w;
        lastH_ = screen.h;
        lastMax_ = maxWidth;
        if (same && !forceKey) return false;
        downscale(screen, maxWidth, scaled_);
    }
    const Image& img = scale ? scaled_ : screen;
    if (img.w <= 0 || img.h <= 0 || img.w > 65535 || img.h > 65535 ||
        img.bgra.size() < size_t(img.w) * size_t(img.h) * 4)
        return false;
    bool key = forceKey || prev_.w != img.w || prev_.h != img.h;

    struct Tile { int x, y, w, h; };
    std::vector<Tile> changed;
    for (int ty = 0; ty < img.h; ty += kTile)
        for (int tx = 0; tx < img.w; tx += kTile) {
            int tw = std::min(kTile, img.w - tx), th = std::min(kTile, img.h - ty);
            bool diff = key;
            for (int y = ty; !diff && y < ty + th; ++y) {
                size_t off = (size_t(y) * img.w + tx) * 4;
                diff = std::memcmp(img.bgra.data() + off, prev_.bgra.data() + off, size_t(tw) * 4) != 0;
            }
            if (diff) changed.push_back({tx, ty, tw, th});
        }
    if (changed.empty()) return false;

    Writer w;
    w.buf.reserve(lastPayload_ + lastPayload_ / 4 + 64);
    w.u16(uint16_t(img.w));
    w.u16(uint16_t(img.h));
    w.u8(key ? 1 : 0);
    w.u16(uint16_t(changed.size()));
    // JPEG of each tile, in parallel if there are many (a key frame at 1080p is 135)
    jpgs_.resize(std::max(jpgs_.size(), changed.size()));
    parallelFor(changed.size(), [&](size_t i) {
        thread_local std::vector<uint8_t> rgb(size_t(kTile) * kTile * 3);
        const Tile& t = changed[i];
        for (int y = 0; y < t.h; ++y) {
            const uint8_t* s = img.bgra.data() + (size_t(t.y + y) * img.w + t.x) * 4;
            uint8_t* d = rgb.data() + size_t(y) * t.w * 3;
            for (int x = 0; x < t.w; ++x, s += 4, d += 3) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
        }
        jpgs_[i].clear();
        stbi_write_jpg_to_func(appendJpeg, &jpgs_[i], t.w, t.h, 3, rgb.data(), quality);
    });
    for (size_t i = 0; i < changed.size(); ++i) {
        const Tile& t = changed[i];
        w.u16(uint16_t(t.x)); w.u16(uint16_t(t.y)); w.u16(uint16_t(t.w)); w.u16(uint16_t(t.h));
        w.bytes(jpgs_[i].data(), jpgs_[i].size());
    }
    // The reference image for the next frame: if it was scaled down, the two buffers are swapped
    // (nothing copied); if not, the capture is copied over the previous buffer (which already has the size)
    if (scale) std::swap(prev_, scaled_);
    else prev_ = screen;
    lastPayload_ = w.buf.size();
    payload = std::move(w.buf);
    return true;
}

// What arrives is controlled by another user: everything is limited before allocating memory or decoding
constexpr int kMaxScreenSide = 4096;  // the sender never goes beyond 3840 wide (see /share)

bool ScreenDecoder::apply(const uint8_t* data, size_t n, Image& canvas) {
    Reader r(data, n);
    int w = r.u16(), h = r.u16();
    bool key = r.u8() != 0;
    int count = r.u16();
    if (w <= 0 || h <= 0 || w > kMaxScreenSide || h > kMaxScreenSide) return false;
    int maxTiles = ((w + kTile - 1) / kTile) * ((h + kTile - 1) / kTile);
    if (count > maxTiles) return false;
    if (!key && (canvas.w != w || canvas.h != h)) return false;
    if (key && (canvas.w != w || canvas.h != h)) {
        canvas.w = w;
        canvas.h = h;
        canvas.bgra.assign(size_t(w) * h * 4, 0);
    }
    // First all tiles are read and checked; then they are decoded in parallel. Each one has to be
    // in its cell of the grid and not repeat: that way two threads never write to the same place of the canvas
    struct Job { int tx, ty, tw, th; const uint8_t* jp; size_t jn; };
    std::vector<Job> jobs;
    std::vector<uint8_t> seen(size_t(maxTiles), 0);
    int cols = (w + kTile - 1) / kTile;
    for (int i = 0; i < count; ++i) {
        int tx = r.u16(), ty = r.u16(), tw = r.u16(), th = r.u16();
        size_t jn = 0;
        const uint8_t* jp = r.bytesView(jn);  // without copying the tile
        // Tile inside the canvas, on the grid, at most 128x128 and only once
        if (tx >= w || ty >= h || tx % kTile || ty % kTile || tw <= 0 || th <= 0 || tw > kTile || th > kTile ||
            jn > 4u * 1024 * 1024)
            continue;
        size_t cell = size_t(ty / kTile) * size_t(cols) + size_t(tx / kTile);
        if (seen[cell]) continue;
        seen[cell] = 1;
        jobs.push_back({tx, ty, tw, th, jp, jn});
    }
    parallelFor(jobs.size(), [&](size_t i) {
        const Job& j = jobs[i];
        int iw, ih, comp;
        // Before decoding, the dimensions the JPEG declares are checked: a 30000x30000 "bomb"
        // would ask for gigabytes of memory
        if (!stbi_info_from_memory(j.jp, int(j.jn), &iw, &ih, &comp) || iw <= 0 || ih <= 0 || iw > kTile || ih > kTile)
            return;
        uint8_t* px = stbi_load_from_memory(j.jp, int(j.jn), &iw, &ih, &comp, 4);
        if (!px) return;
        int cw = std::min({iw, j.tw, w - j.tx}), ch = std::min({ih, j.th, h - j.ty});
        for (int y = 0; y < ch; ++y) {
            const uint8_t* s = px + size_t(y) * iw * 4;
            uint8_t* d = &canvas.bgra[(size_t(j.ty + y) * w + j.tx) * 4];
            for (int x = 0; x < cw; ++x, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; }
        }
        stbi_image_free(px);
    });
    return true;
}

}  // namespace eco
