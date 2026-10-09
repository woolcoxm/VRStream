// Host streaming session: capture/encode loop, paced sending, retransmit
// ring, control handling, adaptive bitrate, stats.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "nvenc_encoder.h"
#include "vrstream/congestion.h"
#include "vrstream/packetizer.h"
#include "vrstream/protocol.h"
#include "vrstream/timesync.h"
#include "vrstream/udp_socket.h"

namespace vrstream {

struct HostConfig {
    uint16_t port = kDefaultPort;
    uint32_t fps = 90;
    uint32_t width = 2560;
    uint32_t height = 1440;
    Codec codec = Codec::H264;
    uint32_t bitrateBps = 150'000'000;
    double fecPercent = 8.0;
    uint16_t mtu = 1200;

    // Self-test (loopback client in-process).
    bool selfTest = false;
    double selfTestLossPct = 0.0;
    int selfTestFrames = 300;
    std::string selfTestOut = "out.h264";
    // Transport-only mode: deterministic checksummed payloads instead of
    // NVENC output (validates FEC/NACK/pacing/reassembly without a GPU;
    // the payload layout is verified by the loopback client).
    bool noEncode = false;
    // Receive encoded frames from the SteamVR driver on this loopback port
    // (kDefaultFeedPort). 0 = internal test source.
    uint16_t feedPort = 0;
};

class HostSession {
  public:
    explicit HostSession(HostConfig cfg);
    ~HostSession();

    // Runs the session until `durationSec` elapses or a Disconnect arrives.
    // Returns 0 on success.
    int run(int durationSec);

  private:
    void encodeLoop();
    void sendLoop();
    void recvLoop();
    void controlLoop();  // ABR + console stats
    void runSelfTestClient();

    void sendControlTo(const std::string& addr, uint16_t port, PacketType type,
                       const void* body, size_t bodyLen);
    void handleReceived(const uint8_t* buf, size_t len, const std::string& from,
                        uint16_t fromPort);
    void serviceNacks(const NackRequestMsg& nack, const std::string& addr, uint16_t port);
    void queueVideoDatagrams(std::vector<PacketizedDatagram>& dgrams, uint32_t frameIndex,
                             uint64_t frameStartUs);

    HostConfig cfg_;
    UdpSocket sock_;
    NvencEncoder encoder_;
    FrameSender packetizer_;
    CongestionController congestion_;

    std::string clientAddr_;
    uint16_t clientPort_ = 0;
    std::atomic<uint32_t> streamId_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<bool> keyframeRequested_{false};
    std::atomic<uint32_t> frameIndex_{0};

    struct PacedSend {
        uint64_t sendAtUs;
        std::vector<uint8_t> datagram;
    };
    std::deque<PacedSend> sendQueue_;
    std::mutex sendMx_;
    std::condition_variable sendCv_;

    // Retransmit ring: frameIndex -> (key -> raw datagram).
    struct RingKey {
        uint16_t groupIdx;
        uint16_t packetInGroup;
        uint8_t isFec;
        bool operator<(const RingKey& o) const {
            return std::tie(groupIdx, packetInGroup, isFec) <
                   std::tie(o.groupIdx, o.packetInGroup, o.isFec);
        }
    };
    std::map<uint32_t, std::map<RingKey, std::vector<uint8_t>>> retransmitRing_;
    std::mutex ringMx_;
    std::atomic<double> fecPercent_;

    // Stats.
    struct Stats {
        std::atomic<uint64_t> sentPackets{0}, sentBytes{0}, retxPackets{0};
        std::atomic<uint32_t> encodedFrames{0};
        std::atomic<uint64_t> encodeTimeUsTotal{0};
        std::atomic<uint64_t> encodeBytesTotal{0};
        std::mutex rxMx_;
        StatsReportMsg lastReport{};
        uint64_t lastReportAtUs = 0;
        ClockSync sync;
    } stats_;

    uint64_t lastIdrUs_ = 0;

    // Driver feed (encoded frames from the SteamVR driver process).
    struct FedFrame {
        uint64_t ptsUs = 0;
        std::vector<uint8_t> annexB;
    };
    UdpSocket feedSock_;
    std::thread feedThread_;
    std::mutex feedMx_;
    std::condition_variable feedCv_;
    std::deque<FedFrame> feedQueue_;
    std::map<uint32_t, std::vector<std::vector<uint8_t>>> feedFrags_;
    std::map<uint32_t, uint64_t> feedPts_;
    void feedLoop();
};

}  // namespace vrstream
