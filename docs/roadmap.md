# Roadmap

Ordered by impact on the goal (wire-quality latency first, then quality).

## Bring-up (on target hardware)

1. ~~Fix the NVIDIA driver on the dev PC~~ **DONE 2026-10-09** — the RTX 3060
   was physically absent; card installed, driver 591.59 bound.
2. ~~NVENC smoke test~~ **DONE 2026-10-09** — encode self-test on the 3060:
   H.264 High + HEVC Main, 2560x1440, 450/450 frames 0 corrupt, ~85fps,
   ffmpeg full-decode exit 0. Encode (incl. CPU texture upload) ~8.5 ms/frame;
   GPU-only path (driver/encodeGpu) should pipeline lower.
3. **Quest client on-device** (needs the headset plugged in / wireless adb):
   sideload, launch, connect to the host test pattern; check MediaCodec
   low-latency keys took effect (vendor key visible in logcat), measure
   decode→submit from the stats path.
4. **SteamVR driver bring-up** (needs Steam+SteamVR installed): register with
   vrpathreg, launch vrserver with VR_LOG, watch the driver log lines; then
   run a real game end-to-end.

## Latency (v0.2)

5. ~~Pose pipeline / timewarp contract~~ **DONE 2026-10-09 (code)** —
   VideoMeta packet carries the SteamVR render poses/FOVs to the client,
   which submits them in the projection layer (LOCAL fallback on loss).
   Remaining refinement: exact frame-index pairing through the decoder queue
   (currently newest-meta-with-newest-frame), and pose-history matching on
   the driver side if SteamVR poses and layers ever skew.
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
