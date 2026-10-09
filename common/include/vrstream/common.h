// VRStream common core - basic types and time helpers shared by host and client.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace vrstream {

inline uint64_t nowUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline uint64_t nowMs() { return nowUs() / 1000; }

template <typename T>
inline T clampv(T v, T lo, T hi) {
    return std::max(lo, std::min(hi, v));
}

// Exponentially weighted moving average (safe for the first samples).
class Ewma {
  public:
    explicit Ewma(double alpha) : alpha_(alpha) {}
    void update(double v) {
        if (!init_) {
            value_ = v;
            init_ = true;
        } else {
            value_ = alpha_ * v + (1.0 - alpha_) * value_;
        }
    }
    double value() const { return value_; }
    bool initialized() const { return init_; }

  private:
    double alpha_;
    double value_ = 0.0;
    bool init_ = false;
};

}  // namespace vrstream
