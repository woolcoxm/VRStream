// Adaptive bitrate controller for the video stream.
//
// Unlike TCP-friendly congestion control, VR streaming owns a dedicated link
// (dedicated AP, single client) and optimizes for: never queue (rising delay
// = packets missing their frame deadline), never starve the decoder. Inputs
// come from the client's periodic StatsReport plus host-side send stats.
//
// Behavior: back off FAST on loss spikes or decoder backlog, ramp up SLOWLY
// when the link looks clean, hard cap at the configured ceiling.
#pragma once

#include <algorithm>

#include "vrstream/common.h"

namespace vrstream {

struct CongestionInputs {
    float lossPercent = 0.f;      // recent packet loss at the receiver
    float goodputMbps = 0.f;      // measured received goodput
    float oneWayDelayTrend = 0.f; // >0 when delay is rising (queue building)
    uint32_t decodeBacklog = 0;   // frames queued at the decoder
    float frameAgeAtDisplayMs = 0.f;
};

class CongestionController {
  public:
    struct Config {
        uint32_t minBitrateBps = 20'000'000;    // 20 Mbps
        uint32_t maxBitrateBps = 400'000'000;   // 400 Mbps (5 GHz practical)
        uint32_t startBitrateBps = 80'000'000;  // 80 Mbps
        float lossBackoffFactor = 0.6f;
        float backlogBackoffFactor = 0.7f;
        float rampUpFactor = 1.05f;             // +5% per decision tick
        uint64_t decisionIntervalMs = 500;
        float lossThresholdPct = 1.0f;
        float delayTrendThreshold = 0.15f;
    };

    explicit CongestionController(Config cfg) : cfg_(cfg), bitrate_(cfg.startBitrateBps) {}
    CongestionController() : CongestionController(Config{}) {}

    // Called every decisionIntervalMs with fresh receiver stats.
    uint32_t update(const CongestionInputs& in) {
        if (in.decodeBacklog >= 2) {
            // Decoder drowning: cut bitrate; buffering would add latency.
            bitrate_ = static_cast<uint32_t>(bitrate_ * cfg_.backlogBackoffFactor);
        } else if (in.lossPercent > cfg_.lossThresholdPct) {
            // Higher loss above threshold => deeper backoff (inverse scale).
            float scale = std::max(1.f, in.lossPercent / cfg_.lossThresholdPct);
            bitrate_ = static_cast<uint32_t>(bitrate_ * cfg_.lossBackoffFactor / scale);
        } else if (in.oneWayDelayTrend > cfg_.delayTrendThreshold) {
            // Queue building at the AP: ease off before frames die.
            bitrate_ = static_cast<uint32_t>(bitrate_ * 0.85f);
        } else if (in.frameAgeAtDisplayMs > 25.f) {
            bitrate_ = static_cast<uint32_t>(bitrate_ * 0.9f);
        } else {
            bitrate_ = static_cast<uint32_t>(bitrate_ * cfg_.rampUpFactor);
        }
        bitrate_ = clampv<uint32_t>(bitrate_, cfg_.minBitrateBps, cfg_.maxBitrateBps);
        return bitrate_;
    }

    uint32_t bitrate() const { return bitrate_; }
    void forceBitrate(uint32_t bps) {
        bitrate_ = clampv<uint32_t>(bps, cfg_.minBitrateBps, cfg_.maxBitrateBps);
    }

  private:
    Config cfg_;
    uint32_t bitrate_;
};

}  // namespace vrstream
