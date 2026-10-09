# Roadmap

Ordered by impact on the goal (wire-quality latency first, then quality).

## Bring-up (next session, on target hardware)

1. **Fix the NVIDIA driver on the dev PC** — RTX 3060 present but driver
   unbound (nvlddmkm stopped, device not enumerated). Reinstall/rollback the
   driver and reboot; NVENC paths then run for the first time on real silicon.
2. **NVENC smoke test**: `vrstream_host --self-test` (encode mode) → decode
   `out.h264` with ffprobe; verify encode latency per frame against the
   3–5 ms budget.
3. **Quest client on-device**: sideload, launch, connect to the host test
   pattern; check MediaCodec low-latency keys took effect (vendor key visible
   in logcat), measure decode→submit from the stats path.
4. **SteamVR driver bring-up**: register with vrpathreg, launch vrserver with
   VR_LOG, watch the driver log lines; then run a real game end-to-end.

## Latency (v0.2)

5. Pose pipeline: driver-side pose history matching (GetBestPoseMatch) so each
   submitted layer carries the render pose → client submits host poses in the
   projection layer (currently LOCAL poses) — completes the timewarp contract.
6. Host-side pose prediction (constant-velocity now in the wire; upgrade to
   the Kalman filter per the papers digest).
7. Frame scheduling on the client: deadline-aware decode polling against
   `predictedDisplayPeriod` (ALVR multiplier approach) instead of newest-wins.
8. Latency overlay in-headset (stage breakdown like VD 1.18+).

## Quality (v0.3)

9. HEVC 10-bit default mode + AV1 gate on Ada; per-codec bitrate ceilings.
10. Fixed foveated encoding (ALVR FFR-style shader in the driver composite).
11. Audio path (AAudio low-latency out, opus mic in).
12. IDR/RFI policy: reference-frame invalidation instead of IDR on loss
    (Sunshine model), intra-refresh option.

## Hardening (v0.4)

13. Encryption (X25519 + AES-GCM per packet).
14. mDNS discovery + a proper pairing UI on the headset (lobby quad layer).
15. Adaptive prediction horizon from transport p95; controller/hand tracking
    uplink; haptics downlink.
16. USB/ADB transport mode.
