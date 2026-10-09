# VRStream

**Open-source, low-latency PC → Meta Quest 3 VR game streaming.**

VRStream streams SteamVR games from a Windows gaming PC to a Quest 3 over a
dedicated Wi-Fi 6 5 GHz link, with motion-to-photon latency as the first-class
design goal — every millisecond is budgeted ([docs/latency-budget.md](docs/latency-budget.md))
and the packet path is engineered, not incidental ([docs/protocol.md](docs/protocol.md)).

```
┌───────────────────────── PC (ethernet) ─────────────────────────┐   ┌──── Quest 3 (Wi-Fi 6 5GHz) ────┐
│  SteamVR game → driver → NVENC (H.264/HEVC/AV1) → paced UDP ────┼──►│  FEC/NACK recover → MediaCodec │
│                          ▲ pose stream ~250 Hz ─────────────────┼───┤  → OpenXR projection layer     │
└──────────────────────────┴──────────────────────────────────────┘   └────────────────────────────────┘
```

## What makes it different

Measured against the open-source state of the art ([docs/research-oss.md](docs/research-oss.md)):

| | ALVR v21 | WiVRn | **VRStream** |
|---|---|---|---|
| Packet pacing | ✗ (bursts) | ✗ | ✓ spread across 60% of frame interval |
| FEC | ✗ (removed) | ✗ | ✓ adaptive 4–20% MDS Reed-Solomon (any-K-of-K+R) |
| NACK retransmit | ✗ | ✗ | ✓ quiet-window gated, per-fragment |
| Real clock sync | ✗ | ✓ (regression) | ✓ (min-RTT NTP-style) |
| Latency attribution | derived by subtraction | 11-stage timestamps | 11-stage + direct network measurement |
| ABR inputs | delay residual | feedback | loss + decode backlog + delay trend + frame age |

Loss behavior: at 7% random packet loss in the integration self-test, **all
frames still arrive intact** (FEC absorbs most, NACK repairs the rest within
a ~6 ms window) — a single lost packet never kills a frame.

## Status (October 2026)

| Component | State |
|---|---|
| `common/` protocol + FEC + pacing + timesync + ABR | **implemented, 10k+ unit checks, CI-green** |
| `host/` streamer (NVENC, session, paced sender) | **implemented; transport self-test green in CI (no GPU needed)** |
| `host/` NVENC encode | implemented per ALVR/Sunshine study; needs a working NVIDIA driver on the target PC to run |
| `quest/` client (OpenXR + MediaCodec + network) | **implemented, APK builds in CI**; needs on-device bring-up |
| `host/driver/` SteamVR capture driver | **compiles**; needs live vrserver bring-up (DirectMode v9 path) |

See [docs/roadmap.md](docs/roadmap.md) for the ordered path to daily-driver quality.

## Build

### PC host (Windows, VS 2022+, CMake)

```
cmake -B build -S .
cmake --build build --config Release
build\host\Release\vrstream_host.exe --help
```

Runs the transport pipeline with no headset:

```
build\host\Release\vrstream_host.exe --self-test --no-encode --test-loss 5
```

`--self-test` starts an in-process loopback client that reassembles frames,
exercises FEC/NACK/timesync/ABR, verifies every frame's checksum, and writes
the stream to `out.h264`. `--no-encode` uses deterministic payloads so it runs
without an NVIDIA GPU (this is what CI does).

### Quest 3 client

```
cd quest
gradle assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

Run it with the PC address (or without `--es host` to broadcast-discover):

```
adb shell am start -n com.vrstream.client/.MainActivity --es host 192.168.1.50
```

### Streaming (once both sides are on a GPU-equipped PC)

```
:: PC — with the SteamVR driver registered (see below), frames come from games:
vrstream_host.exe --feed-port 9955
:: PC — without SteamVR, built-in test pattern:
vrstream_host.exe --fps 90 --codec h264 --bitrate 150
```

SteamVR driver registration (after building `driver_vrstream.dll`):

```
"C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe" adddriver <repo>\host\driver\driver-root\vrstream
```

## Requirements

- PC: Windows 10/11, NVIDIA GPU with current driver (NVENC). RTX 40+ for AV1.
- Network: PC on ethernet; dedicated Wi-Fi 6 router/AP, 5 GHz, Quest as the
  only client on that radio; TWT disabled on the AP ([docs/research-community.md](docs/research-community.md) §4).
- Open UDP 9944 (stream) and 9955 (loopback driver feed) in Windows Firewall.
- Quest 3: Horizon OS v62+, OpenXR loader 1.0.34 (bundled via Gradle).

## Repository layout

| Path | Contents |
|---|---|
| `common/` | Portable C++20 core: wire protocol, MDS Reed-Solomon FEC, packetizer, pacer, clock sync, congestion control (unit-tested, host+client share it) |
| `host/` | Windows streamer: NVENC encoder, streaming session, self-test; SteamVR driver under `host/driver/` |
| `quest/` | Android/Quest 3 client: OpenXR GLES renderer, async MediaCodec decoder, network core |
| `docs/` | Research digests (papers, community, OSS study, Quest client tech), architecture, protocol spec, latency budget, roadmap |

## Documentation

- [Architecture](docs/architecture.md) — components, threading, design decisions
- [Wire protocol](docs/protocol.md) — every packet, byte-level
- [Latency budget](docs/latency-budget.md) — where the milliseconds go and how each is attacked
- [Research: academic](docs/research-papers.md) · [community/practitioner](docs/research-community.md) · [open-source study](docs/research-oss.md) · [Quest client tech](docs/research-quest-client.md)

## License

MIT — see [LICENSE](LICENSE). Vendored: NVIDIA `nvEncodeAPI.h` (MIT, via FFmpeg nv-codec-headers), Valve `openvr*.h` (BSD).
