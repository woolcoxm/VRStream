// Clock synchronization between headset (client) and PC (server).
//
// Both sides timestamp with monotonic clocks that have an unknown offset.
// The client periodically probes with a 4-timestamp exchange (NTP-style):
//
//   t1 client send --[network a]--> t2 server rx
//   t3 server tx  --[network b]--> t4 client rx
//   rtt = (t4 - t1) - (t3 - t2);  offset = ((t2 - t1) + (t3 - t4)) / 2
//
// Network delay is asymmetric and bursty; the minimum-RTT probe in a sliding
// window gives the tightest offset estimate (classic NTP behavior). Poses and
// video timestamps can then be mapped between clocks.
#pragma once

#include <deque>
#include <utility>

#include "vrstream/common.h"

namespace vrstream {

class ClockSync {
  public:
    struct Sample {
        uint64_t t1, t2, t3, t4;  // microseconds, mixed clocks as above
    };

    void addSample(const Sample& s) {
        uint64_t rtt = (s.t4 - s.t1) - (s.t3 - s.t2);
        int64_t offset =
            (static_cast<int64_t>(s.t2 - s.t1) + static_cast<int64_t>(s.t3 - s.t4)) / 2;
        samples_.push_back({rtt, offset});
        if (samples_.size() > kWindow) samples_.pop_front();
    }

    // Best offset estimate (client clock + offset = server clock), from the
    // probe with the smallest RTT in the window. Returns false before the
    // first sample.
    bool offsetUs(int64_t& out) const {
        if (samples_.empty()) return false;
        const auto* best = &samples_.front();
        for (const auto& s : samples_)
            if (s.first < best->first) best = &s;
        out = best->second;
        return true;
    }

    uint64_t bestRttUs() const {
        if (samples_.empty()) return 0;
        uint64_t best = UINT64_MAX;
        for (const auto& s : samples_) best = std::min(best, s.first);
        return best;
    }

    bool synced() const { return !samples_.empty(); }

  private:
    static constexpr size_t kWindow = 32;
    std::deque<std::pair<uint64_t, int64_t>> samples_;  // (rtt, offset)
};

}  // namespace vrstream
