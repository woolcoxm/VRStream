// VRStream wire protocol v1.
//
// Every UDP datagram starts with BaseHeader. Video frames are split into
// equal-sized data packets (only the logical tail of the frame is shorter,
// delivered zero-padded), which lets Reed-Solomon FEC operate column-wise
// across packets of a group without length ambiguity.
//
// All fields little-endian (both x86 host and Quest 3's ARM are LE).
#pragma once

#include <cstdint>

namespace vrstream {

constexpr uint32_t kMagic = 0x31535256;  // 'VRS1' LE
constexpr uint16_t kDefaultPort = 9944;

enum class PacketType : uint8_t {
    // Reliable control (acked + retransmitted until ControlAck seen).
    HandshakeRequest = 1,
    HandshakeResponse = 2,
    TimeSyncRequest = 3,
    TimeSyncResponse = 4,
    Heartbeat = 5,
    StatsReport = 6,
    NackRequest = 7,
    KeyframeRequest = 8,
    ControlAck = 9,
    Disconnect = 10,
    // Unreliable, high-rate.
    Tracking = 20,
    Haptics = 21,
    // Unreliable video (FEC protected).
    Video = 30,
};

enum VideoFlags : uint8_t {
    kVideoKeyframe = 1 << 0,
    kVideoEndOfStream = 1 << 1,
};

#pragma pack(push, 1)

struct BaseHeader {
    uint32_t magic;
    uint32_t streamId;   // random per session; foreign traffic is dropped
    uint8_t type;        // PacketType
    uint8_t reserved0;   // must be 0
    uint16_t datagramLen;
    uint32_t sequence;   // per-type rolling counter (loss estimation)
    uint64_t timestamp;  // sender steady-clock microseconds
};
static_assert(sizeof(BaseHeader) == 24);

struct VideoHeader {
    uint32_t frameIndex;
    uint16_t groupIdx;   // FEC group index within the frame
    uint16_t dataCount;  // data packets in this group
    uint16_t fecCount;   // repair packets in this group
    uint8_t isFec;       // 0: data packet, 1: repair packet
    uint8_t flags;       // VideoFlags
    uint16_t packetInGroup;  // data index within group (data) or repair index (fec)
    uint32_t payloadOffset;    // byte offset of this packet's payload in the frame
    uint32_t frameBytes;       // logical frame size (without FEC padding)
    uint64_t pts;              // capture timestamp, sender clock, microseconds
};
static_assert(sizeof(VideoHeader) == 30);

struct HandshakeRequestMsg {
    uint32_t protocolVersion;
    uint32_t streamId;
    uint16_t codecMask;   // bit0 h264, bit1 hevc, bit2 hevc10, bit3 av1
    uint16_t refreshRateHz;
    uint16_t eyeWidth;
    uint16_t eyeHeight;
    char deviceName[32];
};
static_assert(sizeof(HandshakeRequestMsg) == 48);

struct HandshakeResponseMsg {
    uint32_t protocolVersion;
    uint32_t streamId;
    uint16_t codec;        // chosen codec
    uint16_t refreshRateHz;
    uint16_t eyeWidth;
    uint16_t eyeHeight;
    uint32_t bitrateBps;
    uint16_t fecPercent;   // initial FEC overhead
    uint16_t mtu;          // max UDP payload the path carries
};
static_assert(sizeof(HandshakeResponseMsg) == 24);

struct TimeSyncRequestMsg {
    uint64_t t1ClientUs;  // client send time
};

struct TimeSyncResponseMsg {
    uint64_t t1ClientUs;   // echoed
    uint64_t t2ServerRxUs; // server receive time, server clock
    uint64_t t3ServerTxUs; // server send time, server clock
};

struct StatsReportMsg {
    uint32_t frameIndexDisplayed;
    uint32_t lostPacketsTotal;
    uint32_t fecRecoveredFrames;
    uint32_t fecFailedFrames;
    uint32_t nackedPacketsRecovered;
    float rttMs;
    float goodputMbps;        // measured video goodput
    float transportJitterMs;  // EWMA of inter-packet jitter
    uint32_t decodeBacklog;   // frames waiting in decode queue right now
    float decodeMs;           // EWMA decode latency
    float frameAgeAtDisplayMs; // pts -> displayed, mapped to one clock
    uint32_t displayLateFrames;
};

struct NackEntry {
    uint32_t frameIndex;
    uint16_t groupIdx;
    uint16_t packetIdx;
};

struct NackRequestMsg {
    uint8_t count;
    NackEntry entries[32];
};
static_assert(sizeof(NackRequestMsg) == 1 + 32 * 8);

struct PoseSample {
    uint64_t clientTimeUs;
    float px, py, pz;
    float qx, qy, qz, qw;
    float vx, vy, vz;        // linear velocity m/s (for PC-side prediction)
    float avx, avy, avz;     // angular velocity rad/s
};
static_assert(sizeof(PoseSample) == 60);

struct TrackingMsg {
    uint8_t poseCount;
    uint8_t controllerCount;
    PoseSample head[2];  // latest + previous for extrapolation
};
static_assert(sizeof(TrackingMsg) == 122);

struct ControlAckMsg {
    uint8_t ackedType;
    uint32_t ackedSequence;
};

#pragma pack(pop)

constexpr uint32_t kProtocolVersion = 1;

// Loopback feed protocol (SteamVR driver -> host process), one frame per
// fragment set, UDP.
constexpr uint32_t kFeedMagic = 0x46535256;  // 'VRSF'
constexpr uint16_t kDefaultFeedPort = 9955;

#pragma pack(push, 1)
struct FeedPacketHeader {
    uint32_t magic;
    uint32_t frameCounter;
    uint16_t fragIdx;
    uint16_t fragCount;
    uint32_t frameLen;
    uint64_t ptsUs;
};
#pragma pack(pop)
static_assert(sizeof(FeedPacketHeader) == 24);

// Codec ids on the wire.
enum class Codec : uint16_t {
    H264 = 0,
    H265 = 1,
    H26510 = 2,
    Av1 = 3,
};

inline const char* codecName(Codec c) {
    switch (c) {
        case Codec::H264: return "H.264";
        case Codec::H265: return "H.265";
        case Codec::H26510: return "H.265 10-bit";
        case Codec::Av1: return "AV1";
        default: return "?";
    }
}

}  // namespace vrstream
