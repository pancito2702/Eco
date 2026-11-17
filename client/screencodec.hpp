// Lightweight screen codec: the image is split into 128x128 tiles and
// only the ones that changed since the previous frame are sent (as JPEG).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "screen.hpp"

namespace eco {

class ScreenEncoder {
public:
    int maxWidth = 1280;  // rescales if the screen is wider
    int quality = 60;     // calidad JPEG 1-100

    // Returns false if nothing changed (and no key frame was requested)
    bool encode(const Image& screen, bool forceKey, std::vector<uint8_t>& payload);

private:
    Image scaled_, prev_;
    std::vector<std::vector<uint8_t>> jpgs_;  // JPEG of each tile (reused between frames)  // work buffers reused between frames
    uint64_t lastHash_ = 0;           // digest of the last capture (to skip identical ones)
    int lastW_ = 0, lastH_ = 0, lastMax_ = 0;
    bool hashed_ = false;
    size_t lastPayload_ = 0;
};

class ScreenDecoder {
public:
    // Applies a frame to the canvas. Returns false if a key frame is needed.
    bool apply(const uint8_t* data, size_t n, Image& canvas);
};

void downscale(const Image& src, int maxWidth, Image& dst);

}  // namespace eco
