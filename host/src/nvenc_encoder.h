// NVENC hardware encoder wrapper (H.264 / HEVC / AV1), latency-tuned.
//
// Configuration follows the ALVR/WiVRn/Sunshine study in docs/research-oss.md:
// CBR, infinite GOP with manual FORCEIDR, no B-frames, zeroReorderDelay,
// VBV = 1.1 frames with full initial delay, repeatSPSPPS, spatial AQ,
// ultra-low-latency tuning, RGBA textures fed directly (NVENC does the
// RGB->YUV conversion internally for SDR).
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vrstream/protocol.h"

namespace vrstream {

class NvencEncoder {
  public:
    struct Caps {
        bool h264 = false, h265 = false, av1 = false;
        std::string adapterName;
    };

    // Probes NVENC availability on `device`; returns false if the driver or
    // GPU cannot encode. `capsOut` (optional) receives codec support.
    static bool probe(ID3D11Device* device, Caps* capsOut = nullptr);

    NvencEncoder();
    ~NvencEncoder();

    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    bool init(ID3D11Device* device, Codec codec, uint32_t width, uint32_t height,
              uint32_t fps, uint32_t bitrateBps);

    // Encodes one RGBA frame. `srcRgba` is copied into an NVENC-registered
    // texture row by row (rowPitch bytes per row). Appends the Annex-B output
    // to `outAnnexB` (SPS/PPS repeated on keyframes via repeatSPSPPS).
    bool encode(const uint8_t* srcRgba, size_t rowPitch, bool forceIdr,
                uint64_t ptsUs, std::vector<uint8_t>& outAnnexB);

    // GPU-to-GPU variant: copies `src` (same D3D11 device as the encoder)
    // into the registered input texture with CopyResource, then encodes.
    bool encodeGpu(ID3D11Texture2D* src, bool forceIdr, uint64_t ptsUs,
                   std::vector<uint8_t>& outAnnexB);

    // Applies a new CBR bitrate (reconfigure; no session restart).
    void setBitrate(uint32_t bps);

    uint32_t bitrate() const;
    Codec codec() const;
    bool ok() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vrstream
