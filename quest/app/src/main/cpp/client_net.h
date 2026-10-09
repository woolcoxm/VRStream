// Client-side network session: handshake, video reception + reassembly,
// control timers (timesync, stats, NACK), tracking uplink.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "vrstream/packetizer.h"
#include "vrstream/protocol.h"
#include "vrstream/timesync.h"
#include "vrstream/udp_socket.h"

namespace vrstream {

struct ClientNetConfig {
    std::string host = "255.255.255.255";
    uint16_t port = kDefaultPort;
    uint16_t codecMask = 0b1111;
    uint16_t refreshRateHz = 90;
};

class ClientNet {
  public:
    // Called for every completed video frame (Annex-B bytes, pts host clock
    // microseconds, frameIndex, keyframe flag).
    using FrameSink = std::function<void(const uint8_t*, size_t, uint64_t, uint32_t, bool)>;
    // Returns the freshest head pose for the tracking uplink.
    using PoseSource = std::function<PoseSample()>;

    explicit ClientNet(ClientNetConfig cfg) : cfg_((std::move(cfg))) {}

    // Blocking handshake (retries). Fills the negotiated session parameters.
    bool start(const FrameSink& onFrame, const PoseSource& poseSource);
    void stop();

    bool running() const { return running_.load(); }
    const HandshakeResponseMsg& negotiated() const { return negotiated_; }
    ClockSync& sync() { return sync_; }
    uint64_t packetsReceived() const { return packetsReceived_.load(); }

  private:
    void netLoop();
    void controlTick();
    bool doHandshake(const std::string& host);
    void sendPacket(PacketType type, const void* body, size_t len);

    ClientNetConfig cfg_;
    UdpSocket sock_;
    FrameReceiver receiver_;
    ClockSync sync_;
    FrameSink onFrame_;
    PoseSource poseSource_;

    std::string serverAddr_;
    uint16_t serverPort_ = 0;
    uint32_t streamId_ = 0;
    HandshakeResponseMsg negotiated_{};
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::atomic<uint64_t> packetsReceived_{0};
    uint32_t seqTracking_ = 0;
};

}  // namespace vrstream
