// CPU-generated RGBA test pattern with per-frame motion and frame counters.
// Used to validate the encode->packetize->transport->reassemble pipeline
// without SteamVR, and to give the codec something with real entropy
// (gradients, edges, moving high-contrast object).
#pragma once

#include <cstdint>
#include <vector>

namespace vrstream {

class TestSource {
  public:
    TestSource(uint32_t width, uint32_t height) : width_(width), height_(height) {
        pixels_.resize(static_cast<size_t>(width) * height * 4);
    }

    // Renders frame `n` into the internal RGBA buffer; returns pointer +
    // row pitch for direct upload into the encoder texture.
    const uint8_t* render(uint64_t n) {
        const size_t rowBytes = static_cast<size_t>(width_) * 4;
        for (uint32_t y = 0; y < height_; y++) {
            uint8_t* row = pixels_.data() + static_cast<size_t>(y) * rowBytes;
            for (uint32_t x = 0; x < width_; x++) {
                uint8_t* px = row + x * 4;
                // Gradient background that shifts per frame.
                px[0] = static_cast<uint8_t>((x * 255) / width_);
                px[1] = static_cast<uint8_t>((y * 255) / height_);
                px[2] = static_cast<uint8_t>((n * 3) & 0xFF);
                px[3] = 255;
            }
        }
        // Moving white square (easy to eyeball in captured output).
        const uint32_t box = width_ / 12;
        const uint32_t bx = (static_cast<uint32_t>(n * 7) % (width_ - box));
        const uint32_t by = (static_cast<uint32_t>(n * 11) % (height_ - box));
        for (uint32_t y = by; y < by + box; y++) {
            uint8_t* row = pixels_.data() + static_cast<size_t>(y) * rowBytes;
            for (uint32_t x = bx; x < bx + box; x++) {
                uint8_t* px = row + x * 4;
                // 8x8 checkerboard inside the box (high frequency detail).
                bool odd = ((x / 8) + (y / 8)) & 1;
                px[0] = px[1] = px[2] = odd ? 255 : 0;
            }
        }
        // 32x32 high-frequency noise patch in the corner (stress entropy).
        for (uint32_t y = 0; y < 32 && y < height_; y++) {
            uint8_t* row = pixels_.data() + static_cast<size_t>(y) * rowBytes;
            for (uint32_t x = 0; x < 32 && x < width_; x++) {
                uint8_t* px = row + x * 4;
                uint32_t v = (x * 7919u + y * 104729u + static_cast<uint32_t>(n) * 31u);
                px[0] = px[1] = px[2] = static_cast<uint8_t>(v >> 8);
            }
        }
        return pixels_.data();
    }

    size_t rowPitch() const { return static_cast<size_t>(width_) * 4; }

  private:
    uint32_t width_, height_;
    std::vector<uint8_t> pixels_;
};

}  // namespace vrstream
