# VRStream

**Open-source, low-latency PC → Meta Quest 3 VR game streaming.**

VRStream streams SteamVR/OpenXR games from a Windows gaming PC to a Quest 3 over a
dedicated Wi-Fi 6 5 GHz link, with motion-to-photon latency as the first-class design goal.

```
┌───────────────────────── PC (ethernet) ─────────────────────────┐   ┌──── Quest 3 (Wi-Fi 6 5GHz) ────┐
│  SteamVR game → capture → NVENC (H.264/HEVC/AV1) → paced UDP ───┼──►│  FEC recover → MediaCodec →     │
│                                        ▲ pose stream 120 Hz ────┼───┤  OpenXR compositor layer        │
└────────────────────────────────────────┴────────────────────────┘   └─────────────────────────────────┘
```

## Design priorities

1. **Latency** — every millisecond is budgeted and measured (see `docs/latency-budget.md`).
2. **Smart packet management** — paced transmission, forward error correction,
   frame-level NACK, adaptive bitrate driven by decoder backlog. See `docs/protocol.md`.
3. **Quality** — hardware encoding at high bitrates, per-codec tuning, 10-bit where it wins.

## Status

Early development. See `docs/roadmap.md`.

## Repository layout

| Path | Contents |
|---|---|
| `common/` | Shared C++ core: wire protocol, FEC, pacing, clock sync (portable, unit-tested) |
| `host/` | Windows PC streamer: capture, NVENC encode, network, SteamVR driver |
| `quest/` | Android/Quest 3 client: OpenXR renderer, MediaCodec decoder, network |
| `docs/` | Research notes, architecture, protocol spec, latency budget |

## License

MIT — see [LICENSE](LICENSE).
