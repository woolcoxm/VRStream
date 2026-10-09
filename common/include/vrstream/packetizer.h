// Frame packetization and reassembly with per-group FEC and NACK generation.
//
// Sender: FrameSender splits an encoded frame into equal-sized data packets
// (zero-padding the logical tail), groups them into FEC groups bounded by
// GF(255), generates repair packets per group, and emits datagrams.
//
// Receiver: FrameReceiver collects packets per frame, tries FEC recovery,
// emits completed frames, and produces NACK lists for retransmission of
// packets that are still missing (and still time-relevant).
#pragma once

#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "vrstream/common.h"
#include "vrstream/fec.h"
#include "vrstream/protocol.h"

namespace vrstream {

struct PacketizedDatagram {
    VideoHeader header;
    std::vector<uint8_t> payload;  // payloadLen bytes (padded if tail)
};

struct FrameSenderConfig {
    uint16_t mtu = 1200;          // UDP payload bytes per data packet
    double fecPercent = 8.0;      // repair packets per group, as % of data count
    size_t maxGroupSize = 60;     // data packets per FEC group (<= 255 - fec)
};

class FrameSender {
  public:
    explicit FrameSender(FrameSenderConfig cfg = {}) : cfg_(cfg) {}

    // Splits `frame` into FEC-protected datagrams. `frameIndex` and `pts`
    // are copied into every header. `keyframe` sets kVideoKeyframe.
    std::vector<PacketizedDatagram> packetize(const uint8_t* frame, size_t frameBytes,
                                              uint32_t frameIndex, uint64_t pts,
                                              bool keyframe);

    void setFecPercent(double pct) { cfg_.fecPercent = pct; }
    void setMtu(uint16_t mtu) { cfg_.mtu = mtu; }

  private:
    FrameSenderConfig cfg_;
};

struct ReceivedFrame {
    uint32_t frameIndex;
    uint64_t pts;
    bool keyframe;
    std::vector<uint8_t> bytes;  // logical frame bytes (padding stripped)
};

class FrameReceiver {
  public:
    // Ingest one datagram. Returns a completed frame when all packets of the
    // newest frame arrived (or were recovered), that has not been returned yet
    // and is newer than the last delivered frame. Frames older than the last
    // delivered one are dropped as stale.
    std::optional<ReceivedFrame> ingest(const VideoHeader& hdr, const uint8_t* payload,
                                        size_t payloadLen);

    // Packets still missing for undelivered frames (for NACK). A group is
    // only considered NACKable after it has been quiet for `quietUs` since
    // its last received packet — paced frames legitimately take a few ms to
    // finish arriving, and NACKing those only wastes upstream bandwidth.
    std::vector<NackEntry> pendingNacks(uint64_t staleAfterUs,
                                        uint64_t quietUs = 3000) const;

    uint32_t fecRecoveredFrames() const { return fecRecovered_; }
    uint32_t fecFailedFrames() const { return fecFailed_; }
    uint32_t staleDroppedFrames() const { return staleDropped_; }

  private:
    struct GroupState {
        std::vector<std::vector<uint8_t>> data;    // index -> packet (empty = missing)
        std::vector<std::vector<uint8_t>> repairs; // index -> packet (empty = missing)
        size_t received = 0;                       // data + repair packets received
        bool decoded = false;
        uint64_t firstRxUs = 0;
        uint64_t lastRxUs = 0;
        uint16_t dataCount = 0, fecCount = 0;
        uint32_t frameBytes = 0;
        uint64_t pts = 0;
        uint8_t flags = 0;
    };
    struct FrameState {
        uint32_t frameIndex = 0;
        uint64_t pts = 0;
        uint8_t flags = 0;
        uint32_t frameBytes = 0;
        uint16_t groups = 0;
        std::map<uint16_t, GroupState> group;
    };

    bool tryComplete(FrameState& fs);
    void tryFec(GroupState& g);

    std::map<uint32_t, FrameState> frames_;
    uint32_t lastDelivered_ = 0;
    bool haveDelivered_ = false;
    uint32_t fecRecovered_ = 0, fecFailed_ = 0, staleDropped_ = 0;
    size_t packetLen_ = 0;
};

}  // namespace vrstream
