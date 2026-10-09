# Latency budget: motion-to-photon over Wi-Fi

Target environment: PC on ethernet → Wi-Fi 6 router (dedicated 5 GHz) → Quest 3.
Targets from the literature: **MTP ≤ 20 ms** is the gold standard; **25–40 ms**
is what good shipping solutions achieve; network RTT ≤ 20 ms, video-frame RTT
≤ 33 ms, frame loss ≤ 1%.

## Budget at 90 Hz (typical)

| Stage | Budget | Notes / how we hit it |
|---|---|---|
| Client pose sampling + uplink | 1.5 ms | 120 Hz sampling, ~200 B packets, pacing never starves uplink |
| Host: pose map + prediction | 0.2 ms | min-RTT clock offset; constant-time extrapolation |
| Game render (app GPU) | 6–10 ms | not ours to control; SteamVR's own scheduling |
| Composite + D3D11 copy to NVENC | 0.5–1 ms | single CopyResource into registered texture |
| NVENC encode | 2–5 ms | fixed cost even on 4090 (Pimax measurements); low-latency tuning, no B-frames, zero lookahead |
| Packetize + pace + transmit | 4–7 ms | pace across 60% of frame interval; 1 Gbps wired uplink |
| Wi-Fi 6 downlink (single client) | 2–4 ms | dedicated AP, 80 MHz clean channel; A-MPDU bursts |
| FEC recovery when needed | +2–6 ms | repair via FEC = 0, via NACK ≈ 1.5×RTT |
| MediaCodec decode | 3–4 ms (H.264) / 8–12 ms (HEVC) | XR2 Gen 2; vendor low-latency keys; 1–3 frame queue cap |
| Blit to eye swapchains | 0.5–1 ms | GLES blit of AHardwareBuffer/EGLImage |
| Compositor (app submit → photons) | 2–4 ms | panel scanout; timewarp runs in here |

**Sum (excluding game render): ~16–27 ms transport chain**, comparable to
Virtual Desktop's measured splits (encode ~3–4, network ~3–4, decode ~3–4 plus
render+compositor). With game render this lands at 25–40 ms MTP — the good
band — and **timewarp with ~30 ms prediction lead collapses *effective* MTP
to 5–15 ms during continuous motion** (per Air Link instrumentation: 2–13 ms).

## Where we can beat the shipping solutions

1. **Pacing**: ALVR blasts frames back-to-back (bufferbloat/jitter); we spread
   sends — this protects the tail of the latency distribution, which is what
   users actually feel (NeSt-VR: median 9.7 ms but mean 26.9 ms broke QoE).
2. **FEC + NACK hybrid**: ALVR v21/WiVRn have neither — a single lost ~1200 B
   packet kills a 200+ packet frame and triggers a full IDR stall. We repair
   in-place: FEC (0 ms) for ≤ R losses, NACK (~3–8 ms) for the rest, IDR only
   when the reference chain actually breaks.
3. **Direct latency attribution**: 11-stage timestamps + real clock sync (ALVR
   estimates network latency by subtracting six other stages).
4. **Decoder queue discipline**: hard cap 1–3 frames with running-average
   drop-oldest; latency never accumulates silently.

## Measurement (built-in)

Every frame carries host timestamps (pose recv → encode in/out → first/last
send) and the client reports (first/last recv → decode in/out → submit →
predicted display). The host prints a 1 Hz histogram (p50/p95) of:
transport chain, per-stage means, loss%, FEC hit/miss, NACK repairs, decoder
backlog. `docs/protocol.md#StatsReport` lists the wire fields.

## Jitter policy

- Frames late past their deadline are dropped, never shown (no catch-up
  bursts, no growing queues).
- The p95 of the transport chain is the number we optimize; mean is vanity.
- Prediction horizon adapts to measured p95 (tighter streams predict less →
  fewer pose-estimation artifacts).
