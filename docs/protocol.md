# VRStream wire protocol v1

All datagrams are little-endian. Every UDP datagram starts with `BaseHeader`
(24 bytes). One socket (default UDP 9944) carries all streams; small packets
interleave with video shards naturally because the sender paces shards and
takes no cross-packet lock.

```c
struct BaseHeader {          // 24 bytes
    uint32_t magic;          // 0x31535256 'VRS1'
    uint32_t streamId;       // random per session; foreign traffic dropped
    uint8_t  type;           // PacketType
    uint8_t  reserved0;      // 0
    uint16_t datagramLen;    // sanity vs. actual received length
    uint32_t sequence;       // per-type rolling counter (loss estimation)
    uint64_t timestamp;      // sender steady-clock microseconds
};
```

## Packet types

| Type | Id | Reliability | Direction | Purpose |
|---|---|---|---|---|
| HandshakeRequest | 1 | acked | C→S | capabilities: codecs, refresh, eye size, device name |
| HandshakeResponse | 2 | acked | S→C | chosen codec/bitrate/fec/mtu |
| TimeSyncRequest | 3 | unreliable | C→S | t1 |
| TimeSyncResponse | 4 | unreliable | S→C | t1, t2, t3 (NTP-style 4-timestamp exchange) |
| Heartbeat | 5 | unreliable | both | keepalive (500 ms; drop session at 2 s) |
| StatsReport | 6 | unreliable | C→S | per-frame pipeline timestamps + queue depths (drives ABR) |
| NackRequest | 7 | unreliable | C→S | retransmit list of still-relevant packets |
| KeyframeRequest | 8 | unreliable | C→S | decoder starvation / corruption recovery |
| ControlAck | 9 | unreliable | both | acks reliable control packets |
| Disconnect | 10 | acked | both | clean teardown |
| Tracking | 20 | unreliable | C→S | 120 Hz head pose history (+ controllers later) |
| Haptics | 21 | unreliable | S→C | controller haptics |
| Video | 30 | FEC+NACK | S→C | video shards |

## Video framing

```c
struct VideoHeader {         // 30 bytes after BaseHeader
    uint32_t frameIndex;
    uint16_t groupIdx;       // FEC group within the frame
    uint16_t dataCount;      // data packets in this group
    uint16_t fecCount;       // repair packets in this group
    uint8_t  isFec;          // 0 data, 1 repair
    uint8_t  flags;          // bit0 keyframe, bit1 end-of-stream
    uint16_t packetInGroup;  // data idx (data) or repair idx (fec)
    uint32_t payloadOffset;  // byte offset of this payload in the frame
    uint32_t frameBytes;     // logical frame size (without padding)
    uint64_t pts;            // capture time, host clock, microseconds
};
```

Rules:
- A frame is split into **equal-sized data packets** (only the logical tail is
  shorter; it is delivered zero-padded to keep column-wise RS well-defined).
- Data packets are grouped into FEC groups of ≤ 60 data packets (data+repair
  ≤ 255, GF(256) bound). Repair packets are Vandermonde-systematic RS symbols
  over the group (any K of K+R recovers).
- Receiver delivers the **newest** frame that completes before its deadline;
  older incomplete frames are dropped and their missing data packets NACKed
  only while potentially retransmittable-in-time (default stale window 100 ms,
  ALVR semantics).
- Sender keeps a retransmit ring (last N frames' packets) and services NACKs
  ahead of paced video.

## Tracking

```c
struct PoseSample {          // 60 bytes
    uint64_t clientTimeUs;   // client steady clock
    float px, py, pz;        // position, meters
    float qx, qy, qz, qw;    // orientation quaternion
    float vx, vy, vz;        // linear velocity (prediction)
    float avx, avy, avz;     // angular velocity (prediction)
};
struct TrackingMsg { uint8_t poseCount; uint8_t controllerCount; PoseSample head[2]; }
```

Sent at ≥ 120 Hz (3× the 40 ms-era default: we target 90–120 Hz panels). The
host maps `clientTimeUs` into its clock via the min-RTT offset estimate and
predicts the pose at the frame's target display time.

## Clock sync

NTP-style: client sends `TimeSyncRequest{t1}`; server responds immediately
`{t1, t2=serverRx, t3=serverTx}`; client records `t4`. 
`rtt = (t4-t1) - (t3-t2)`, `offset = ((t2-t1)+(t3-t4))/2`. The estimator keeps
a 32-sample window and uses the min-RTT sample's offset (tightest bound).
Probes every 250 ms plus a burst at session start.

## Adaptive bitrate inputs (StatsReport)

Receiver reports every frame interval: RTT, goodput, transport jitter EWMA,
decode backlog, decode latency EWMA, frame-age-at-display (via clock offset),
loss/fec counters. Host feeds these to `CongestionController` at 2 Hz:
decoder backlog ≥ 2 or loss > 1% or delay-trend high → fast backoff; clean →
+5% ramp; hard bounds 20–400 Mbps.

## Security

v1 assumes a trusted LAN (like ALVR v21): 64-bit random `streamId` filters
stray traffic. Encryption (X25519 ECDH + AES-GCM per packet, WiVRn-style) is
planned; the protocol reserves `reserved0` and per-type extension space so it
can be added without a version break.

## Discovery

Broadcast UDP on port 9944 with a `VRS1-DISC` magic + JSON payload, plus
manual IP entry. (mDNS later.)
