# VRStream architecture

## Components

```
┌────────────────────────── PC host (Windows, ethernet) ──────────────────────────┐
│                                                                                 │
│  capture sources ──► D3D11 staging ──► NVENC encoder ──► packetizer ──► pacer ──┼──┐
│   • test pattern (validation)      RGBA tex,        H.264/HEVC/   RS-FEC    token│  │
│   • DXGI desktop duplication       2 buffers/      AV1 CBR,       groups    bucket│  │
│   • SteamVR driver (games)         no B-frames     infinite GOP          + re-xmit│  │
│                                                        │                          │  │
│  session control  ◄── control+stats+NACK+timesync ────┘                          │  │
│   • handshake, ABR (congestion.h), IDR scheduling                                │  │
│   • pose history + prediction (client Kalman samples)                            │  │
└──────────────────────────────────────────────────────────────────────────────────┘  │
                                                                          UDP 9944   │
                                                                                     ▼
┌────────────────────────────── Quest 3 client (Wi-Fi 6, 5 GHz) ────────────────────┐
│  network thread ──► reassembly (FrameReceiver) ──► AMediaCodec (async, low-lat)  │
│      ▲                    + FEC decode + NACK gen        │ surface mode          │
│      │ feedback (11-stage timestamps, per frame)          ▼                       │
│  pose thread (≥3× refresh) ──► AImageReader queue (cap 1–3 frames)               │
│      ▲                                           │ AHardwareBuffer               │
│  OpenXR session (GLES) ◄── blit to eye swapchains ▼                               │
│  XrCompositionLayerProjection submitted with the PC's render poses + frame time   │
│  ⇒ Quest compositor timewarps with latest head pose (late latching for free)     │
└───────────────────────────────────────────────────────────────────────────────────┘
```

## Latency-first decisions

| Decision | Rationale |
|---|---|
| Single UDP socket, multiplexed streams | tracking/acks interleave with video shards (ALVR model); no head-of-line blocking, no TCP inflight caps |
| Hybrid FEC + NACK | FEC absorbs small losses with zero added RTT; NACK repairs what FEC can't while the frame is still fresh (repair cost ≈ 1.5×RTT ≈ 3–8 ms on a dedicated AP) |
| Paced sends (spread across ~60% of frame interval) | bursts overflow the AP's airtime queue → jitter + missed deadlines (ALVR's known gap; Sunshine/Moonlight pace for this reason) |
| No jitter buffer; newest-frame-wins | stale frames are dropped at the receiver, never displayed late |
| Adaptive bitrate driven by decoder backlog + loss + delay trend | back off before frames die; NeSt-VR-style asymmetric ladder |
| Compositor-layer submission with PC render poses | timewarp hides 20–30 ms of transport latency (the single biggest smoothness lever) |
| Direct measurement via clock sync + 11-stage timestamps | ALVR derives network latency by subtracting six other stages — compounding error; we measure each stage |

## Threading (host)

- **capture/encode**: source frame → D3D11 copy → `EncodeFrame` (blocking, ≤1 frame in flight) → NAL → packetize
- **send**: pacer deadlines + retransmit ring servicing (NACKed packets cut the line)
- **recv**: control channel, stats, NACK, timesync, tracking
- **main**: session state machine, ABR at 2 Hz, console overlay stats

## Threading (client)

- **network**: recv loop → reassembly/FEC → decoder input queue; generates NACKs
- **decoder callbacks**: async AMediaCodec → AImageReader listener → frame queue (cap, drop-oldest)
- **render**: xrWaitFrame → pick newest decoded frame ≤ deadline → blit both eyes → submit projection layer
- **pose**: 3× refresh sampling with prediction timestamps, tiny UDP packets

## Capture path strategy

1. **Test pattern** (in-tree): validates the whole pipeline with no dependencies.
2. **DXGI Desktop Duplication**: captures a monitor; useful for 2D games and continuous integration.
3. **SteamVR driver** (`IVRDriverDirectModeComponent`): per-eye shared textures with keyed mutex → composited → NVENC; the path real VR games take. Driver DLL and streamer communicate over a shared-memory ring.

## Bitrate/codec policy (from research)

- Default: **H.264 @ 150 Mbps** (lowest decode latency, biggest stable ceiling), CBR, CAVLC default with CABAC option.
- Quality mode: **HEVC 10-bit @ 100–150 Mbps** (no banding).
- AV1 gated on Ada+ GPU and Quest 3 (8-bit only).
- ABR bounds: 20–400 Mbps, ramp +5%/500 ms, emergency −40% on loss spikes or decode backlog ≥ 2.
- FEC: adaptive 4–20% by measured loss; pacing spread 60% of frame interval.
