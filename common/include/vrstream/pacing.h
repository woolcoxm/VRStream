// Token-bucket packet pacer.
//
// VR frame deadlines are rigid (~11 ms at 90 Hz): dumping a frame's packets
// in one burst floods the AP's airtime queue, latency spikes, and frames miss
// their deadline. The pacer spreads each frame's datagrams evenly across a
// configurable fraction of the frame interval, with a small extra holding
// budget so control/pose packets are never starved.
#pragma once

#include <cstdint>

namespace vrstream {

class Pacer {
  public:
    // `frameIntervalUs` is the streaming frame period; packets of a frame are
    // spread across `spread` of that period (e.g. 0.6).
    void configure(uint64_t frameIntervalUs, double spread) {
        intervalUs_ = frameIntervalUs;
        spread_ = spread;
    }

    // Next permitted send time for packet number `n` (0-based) of a frame.
    uint64_t sendTimeUs(uint64_t frameStartUs, size_t packetIndex) const {
        // Reserve a headroom slice for control traffic, then spread evenly.
        uint64_t window = static_cast<uint64_t>(intervalUs_ * spread_);
        uint64_t per = window / static_cast<uint64_t>(packetsPerFrame_ ? packetsPerFrame_ : 1);
        return frameStartUs + per * static_cast<uint64_t>(packetIndex);
    }

    void setPacketsPerFrame(size_t n) { packetsPerFrame_ = n; }

    // How long after `nowUs` the next packet may go out, 0 if immediately.
    static uint64_t waitUs(uint64_t now, uint64_t sendAt) {
        return now >= sendAt ? 0 : sendAt - now;
    }

  private:
    uint64_t intervalUs_ = 11111;  // 90 Hz default
    double spread_ = 0.6;
    size_t packetsPerFrame_ = 1;
};

}  // namespace vrstream
