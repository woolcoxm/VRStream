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
    // row pitch for direct upload into the encoder texture. The expensive
    // full-frame pass runs every 8th frame; motion updates are cheap regions.
    const uint8_t* render(uint64_t n) {
        const size_t rowBytes = static_cast<size_t>(width_) * 4;
        if (n % 8 == 1) {
            for (uint32_t y = 0; y < height_; y++) {
                uint8_t* row = pixels_.data() + static_cast<size_t>(y) * rowBytes;
                for (uint32_t x = 0; x < width_; x++) {
                    uint8_t* px = row + x * 4;
                    px[0] = static_cast<uint8_t>((x * 255) / width_);
                    px[1] = static_cast<uint8_t>((y * 255) / height_);
                    px[2] = static_cast<uint8_t>((n * 3) & 0xFF);
                    px[3] = 255;
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
        }
        // Moving checkerboard square (easy to eyeball, cheap to redraw).
        const uint32_t box = width_ / 12;
        const uint32_t bx = (static_cast<uint32_t>(n * 7) % (width_ - box));
        const uint32_t by = (static_cast<uint32_t>(n * 11) % (height_ - box));
        const uint32_t pbx = (static_cast<uint32_t>((n - 1) * 7) % (width_ - box));
        const uint32_t pby = (static_cast<uint32_t>((n - 1) * 11) % (height_ - box));
        auto drawBox = [&](uint32_t x0, uint32_t y0, uint64_t seed) {
            for (uint32_t y = y0; y < y0 + box; y++) {
                uint8_t* row = pixels_.data() + static_cast<size_t>(y) * rowBytes;
                for (uint32_t x = x0; x < x0 + box; x++) {
                    uint8_t* px = row + x * 4;
                    bool odd = ((x / 8) + (y / 8)) & 1;
                    px[0] = px[1] = px[2] = odd ? 255 : 0;
                }
            }
            (void)seed;
        };
        // Erase previous position (background gradient), draw current.
        restoreGradient(pbx, pby, box);
        drawBox(bx, by, n);
        return pixels_.data();
    }

    size_t rowPitch() const { return static_cast<size_t>(width_) * 4; }

  private:
    void restoreGradient(uint32_t x0, uint32_t y0, uint32_t box) {
        const size_t rowBytes = static_cast<size_t>(width_) * 4;
        for (uint32_t y = y0; y < y0 + box && y < height_; y++) {
            uint8_t* row = pixels_.data() + static_cast<size_t>(y) * rowBytes;
            for (uint32_t x = x0; x < x0 + box && x < width_; x++) {
                uint8_t* px = row + x * 4;
                px[0] = static_cast<uint8_t>((x * 255) / width_);
                px[1] = static_cast<uint8_t>((y * 255) / height_);
                px[3] = 255;
            }
        }
    }


  private:
    uint32_t width_, height_;
    std::vector<uint8_t> pixels_;
};

}  // namespace vrstream
