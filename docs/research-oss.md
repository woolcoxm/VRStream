# Open-source reference implementations: engineering report (October 2026)

Repos studied (shallow clones): ALVR `v21.0.0-dev` (Rust + C++ SteamVR driver), WiVRn master (C++, Monado-based), Sunshine master (C++, Moonlight host), moonlight-android master. Paths relative to each clone root.

## 1. ALVR (PC VR streaming to Quest)

ALVR v21 is a Rust workspace with C++ confined to the SteamVR driver (`alvr/server_openvr/cpp/`). Key crates: `alvr/packets`, `alvr/sockets`, `alvr/server_core`, `alvr/client_core`, `alvr/client_openxr`, `alvr/graphics`.

### 1.1 Server frame pipeline

```
Game/SteamVR (vrserver, own process)
  |  eye textures = DXGI shared textures (keyed mutex)
  v
OvrDirectModeComponent::SubmitLayer()        [win32/OvrDirectModeComponent.cpp]
  + PoseHistory::GetBestPoseMatch(pose)      -> recovers targetTimestampNs from pose history
OvrDirectModeComponent::Present(syncTexture)
  + pKeyedMutex->AcquireSync(0,10)/ReleaseSync(0)
  + CD3DRender::GetSharedTexture() -> OpenSharedResource
  + FrameRender::RenderFrame()               [win32/FrameRender.cpp: composite <=10 layers,
                                              color correction, FFR compress, RGB->YUV for HDR]
  + CEncoder::NewFrameReady()                (event)
  v
CEncoder::Run()                              [win32/CEncoder.cpp, THREAD_PRIORITY_MOST_URGENT]
  + IDRScheduler::CheckIDRInsertion()
  v
VideoEncoder::Transmit()  -- tries in order: VideoEncoderAMF -> VideoEncoderNVENC -> VideoEncoderVPL -> VideoEncoderSW
  v
ParseFrameNals()                             [alvr_server/NalParsing.cpp: strip AUD, config NALs]
  -> VideoSend()  (FFI into Rust; attaches head_pose * local_view_params => global_view_params[2])
  v
server_core/src/connection.rs video_send_thread
  -> StreamSender<VideoPacketHeader>.send_header_with_payload()  [stream VIDEO=3]
  -> MultiplexedUdpWriter::send()            [sockets/src/stream_socket/udp.rs: shard into 1400B]
```

**GPU frame path**: no CUDA. The SteamVR driver process opens the compositor's eye textures via `ID3D11Device::OpenSharedResource` (shared handles created in `CreateSwapTextureSet`, guarded by `IDXGIKeyedMutex`), composites with its own D3D11 shaders, then `CopyResource` into the encoder-registered input texture:
- NVENC: `NvEncoderD3D11` allocates input textures (`D3D11_BIND_RENDER_TARGET`) and registers them with `NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX`; `Transmit()` does `GetNextInputFrame()` + `CopyResource` + blocking `EncodeFrame()`.
- AMF: `m_amfContext->InitDX11(device)`, `AllocSurface(AMF_MEMORY_DX11, RGBA)` + `CopyResource`, then `AMFVideoConverter` (RGBA→NV12, or →R10G10B10A2 for 10-bit) → optional `AMFPreProcessing` → encoder.

### 1.2 Exact NVENC configuration

`platform/win32/VideoEncoderNVENC.cpp::FillEncodeConfig()` + defaults from `alvr/session/src/settings.rs`:

| Parameter | Value |
|---|---|
| Codec | H264 / HEVC / AV1 (`NV_ENC_CODEC_*_GUID`) |
| Input format | `NV_ENC_BUFFER_FORMAT_ABGR` SDR (RGBA straight into NVENC), `ABGR10` 10-bit, `NV12`/`YUV420_10BIT` only for HDR |
| Preset/tuning | default **P1 + `NV_ENC_TUNING_INFO_LOW_LATENCY`** via `CreateDefaultEncoderParams()` (1=HighQuality, 2=LowLatency, 3=UltraLowLatency) |
| GOP | `gopLength = NVENC_INFINITE_GOPLENGTH`; IDR forced manually with `picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR` |
| B-frames | `frameIntervalP = 1` (P-frames only) |
| Rate control | `NV_ENC_PARAMS_RC_CBR` (VBR optional); `lowDelayKeyFrameScale = 1` |
| Multipass | `NV_ENC_MULTI_PASS_QUARTER_RESOLUTION` |
| AQ | Spatial AQ default on (`enableAQ=1`) |
| VBV | `vbvBufferSize = vbvInitialDelay = (bitrate_bps / refreshRate) * 1.1` — 1.1 frames |
| Bitrate | `averageBitRate = maxBitRate = bitrate_bps` |
| H264 extras | `repeatSPSPPS=1`, entropy coding default **CAVLC** (CABAC optional — comment: "significantly slower, runaway latency"), optional intra-refresh, `maxNumRefFrames=0`, full-range + BT.709/BT.2020 VUI with sRGB transfer |
| AV1 extras | `repeatSeqHdr=1`, `chromaFormatIDC=1`, IVF wrapper stripped |
| Buffers | `NvEncoderD3D11(device,w,h,fmt, nExtraOutputDelay=0)` → **1 buffer → fully synchronous encode** |
| Reconfig | every frame check; ABR change → `Reconfigure()` without session restart |

### 1.3 Exact AMF configuration

- All codecs: `USAGE = *_ULTRA_LOW_LATENCY`.
- H264: `PROFILE LEVEL 42`, `B_PIC_PATTERN=0`, `IDR_PERIOD=0`, `INSERT_AUD=false`, `MAX_NUM_REFRAMES=0`, `VBV_BUFFER_SIZE = bitrate/fps*1.1`, `FULL_RANGE_COLOR=true`; CBR or `LATENCY_CONSTRAINED_VBR`.
- HEVC: `GOP_SIZE=0`, `NUM_GOPS_PER_IDR=0`, 10-bit via `COLOR_BIT_DEPTH_10` + `PROFILE_MAIN_10`.
- AV1: `GOP_SIZE=0`, `VBV = bitrate/fps*1.2`, CAQ.
- `QUERY_TIMEOUT=1000` (ms) when caps support it; synchronous `QueryOutput()` polling.
- Dynamic bitrate: `SetProperty(TARGET/PEAK_BITRATE)` + IDR if `amdBitrateCorruptionFix`.

### 1.4 Network protocol (v21 wire format)

**Two sockets**: control = **TCP :9943** (`CONTROL_PORT`), bincode over length-prefixed frames (`u32` LE length + payload), `TCP_NODELAY`. Stream = **UDP :9944** (`packet_size=1400`) or TCP when wired. mDNS `_alvr._tcp.local.` with `protocol`/`device_id` TXT keys. KeepAlive 500 ms, 2 s timeout. **No encryption anywhere in v21-dev.**

**UDP shard prefix** (`sockets/src/stream_socket/udp.rs`, `SHARD_PREFIX_SIZE = 14`), little-endian:

```c
struct AlvrShardHeader {      // 14 bytes, at offset 0 of every UDP datagram
    uint16_t stream_id;       // 0=TRACKING 1=HAPTICS 2=AUDIO 3=VIDEO 4=STATISTICS
    uint32_t packet_index;    // per-stream monotonic, wraps
    uint32_t shards_count;    // datagrams for this logical packet
    uint32_t shard_index;     // 0..shards_count-1
};
// followed by: bincode of the stream's typed header, then raw payload
```

One encoded video frame = one logical packet = `ceil(payload / (1400-14))` datagrams sent back-to-back. The socket write lock is taken **per shard**, so small packets (tracking/audio) interleave between video shards.

Video typed header `VideoPacketHeader` (`packets/src/lib.rs`): `timestamp: Duration`, `global_view_params: [ViewParams;2]` (pose Vec3+Quat; fov 4 floats), `foveation_center_shifts: Option<[[f32;2];2]>`, `is_idr: bool`, then Annex-B NAL bytes.

**Reliability**: **no FEC, no NACK, no retransmission** (v21 removed the old Reed-Solomon FEC). Loss → gap detection via wrapping `packet_index` → `had_packet_loss` → drop until next IDR + `ClientControlPacket::RequestIdr` over TCP. Server `IDRScheduler` rate-limits IDRs. **No pacing logic**: shards sent as fast as `send()` allows.

**Adaptive bitrate** (`server_core/src/bitrate.rs::BitrateManager`), 1 s updates:

```
throughput_bps = avg_packet_bytes * 8 * saturation_multiplier / avg_network_latency
```

where `network_latency` is **derived by subtraction**: `total_pipeline_latency - (game_latency + server_compositor + encoder + decode + decoder_queue + rendering + vsync_queue)` — compounding measurement error. Optional limiters on decoder latency, network latency, encoder saturation.

### 1.5 Client (Quest) pipeline

```
UDP :9944 -> MultiplexedUdpReader::recv()      [reassemble shards]
 -> StreamReceiver<VideoPacketHeader>::recv()  [loss detection -> RequestIdr over TCP]
 -> VideoDecoderSink::push_frame_nal()
     AImageReader(AMediaCodec surface mode)    [client_core/src/video_decoder/android.rs]
 -> MediaCodec (NDK) -> AHardwareBuffer queue (maxImages=10, PRIVATE format)
 -> client_openxr render loop: dequeue_frame()
 -> StreamRenderer::render()
     AHardwareBuffer -> EGL image -> GL_TEXTURE_EXTERNAL_OES (staging.rs)
     -> wgpu/GLES render into OpenXR swapchain images
        (foveation decompression, upscaling, sRGB fix, reprojection
         using VideoPacketHeader.global_view_params)
 -> xrEndFrame -> per-frame ClientStatistics on STATISTICS stream
```

MediaCodec exact facts (`android.rs` + `settings.rs`):
- `AMediaCodec_createDecoderByType("video/avc"|"video/hevc"|"video/av01")`, **surface mode** into `AImageReader_new_with_usage(1,1, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, maxImages=10)` (1x1 placeholder; width/height hardcoded).
- Keys: `operating-rate = i32::MAX`, `priority = 0`, `vendor.qti-ext-dec-low-latency.enable = 1`. `"low-latency"=1` **commented out** ("Android smartphones crash enabling this").
- Output thread calls `releaseOutputBuffer_at_time(presentation_time)`; listener pushes `AHardwareBuffer` + timestamp into a queue capped at `2 * max_buffering_frames`, running-average drop policy.
- Frame metadata travels in the video header, 128-entry history keyed by timestamp, matched at compositor start.

### 1.6 Pose path & timestamps

- Client `stream_input_loop` runs at **frame_interval / 3** (3x refresh rate), sends `TrackingData {poll_timestamp = now + max_prediction, device_motions (head + hands with velocities), hand_skeletons, face, body}`.
- **No clock sync between PC and headset** (explicit comment). Timestamps are headset-clock `XrTime`; server uses them as frame IDs. `OvrDirectModeComponent::SubmitLayer` recovers which tracking sample produced a frame by matching the pose matrix against `PoseHistory` (`GetBestPoseMatch`, `MATCH_WINDOW_NS = 250 ms`).
- Wired clients switch stream socket to TCP automatically.

## 2. WiVRn (OpenXR/Monado)

C++; server embeds **Monado's compositor** and runs as an OpenXR driver — PC apps see a native OpenXR runtime, no SteamVR/OpenVR.

```
OpenXR app -> Monado compositor -> wivrn driver
  server/compositor/compositor.cpp  (layers, foveation, layer_squasher)
  server/compositor/pacer.cpp       (display-time prediction, wake-up margins)
  server/encoder/video_encoder_nvenc.cpp | video_encoder_vulkan_h264/h265 | x264 | raw
    -> shards (common/wivrn_packets.h video_stream_data_shard, payload <=1400B)
    -> server/driver/wivrn_connection.cpp (UDP; OpenSSL EVP stream encryption)
  client: client/decoder (Android MediaCodec -> AHB -> Vulkan) + shard_accumulator
    -> client/scenes/stream.cpp (OpenXR session) + stream_defoveator
```

Differences vs ALVR:
- **Two eyes encoded as separate streams** (left, right, optional alpha) with `stream_item_idx`.
- **Encryption built-in**: X25519/X448 ECDH key agreement, per-packet OpenSSL-EVP AEAD on TCP and UDP.
- **Real clock sync**: `server/driver/clock_offset.cpp` — `timesync_query` every 10 ms until 100 samples then 100 ms; `headset_time = server_time + b` via **linear regression** on midpoint times, discards RTT > 3x mean.
- **Server-controlled tracking cadence**: `tracking_control` packet with per-device sampling pattern (`device + prediction_ns`); client samples each device at its optimal phase within the frame period.
- **Eye-tracked foveated encoding** (`server/compositor/foveation.cpp`), compact ratio description shipped in first shard's `view_info`.
- **Rich per-frame telemetry**: `feedback` packet with 11 timestamps (encode_begin/end, send_begin/end, received_first/last_packet, sent_to_decoder, received_from_decoder, blitted, displayed) + `times_displayed`.
- **RFI-aware IDR state machine**: `server/encoder/idr_handler.cpp` — only requests IDR when a dropped frame was a reference frame.
- NVENC: preset **P4**, `NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY`, CBR, `vbvBufferSize = 2 frames`, `vbvInitialDelay = 1 frame`, `enableLookahead=0`, `multiPass = TWO_PASS_QUARTER_RESOLUTION`, `enableAQ=1`, `enableNonRefP=1`, infinite GOP, `frameIntervalP=1`, `repeatSPSPPS=1`, `maxNumRef*=0`.
- Android decoder: **async MediaCodec callbacks** (`AMediaCodec_setAsyncNotifyCallback`), `AImageReader` `maxImages = image_buffer_size + 4`, `OPERATING_RATE = ceil(frame_rate)`, `PRIORITY = 0`; Qualcomm vendor key commented out. Vulkan import via `vkGetAndroidHardwareBufferPropertiesANDROID`.
- Discovery Avahi/mDNS. No FEC; loss → feedback → IDR.

## 3. Sunshine (game streaming host)

HTTPS pairing, RTSP handshake, **ENet reliable-UDP control** + raw UDP video/audio.

NVENC session (`src/nvenc/nvenc_base.cpp`):
- `tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY`, preset P1–P7 from config.
- `enablePTD = 1`; `rcParams.zeroReorderDelay = 1`; CBR; `enableLookahead=0`; `lowDelayKeyFrameScale=1`; `vbvBufferSize = bitrate/framerate (+ vbv_percentage_increase%)`.
- **Slicing**: `sliceMode = 3`, `sliceModeData = slicesPerFrame` (parallel loss recovery).
- **Intra-refresh option**: `enableIntraRefresh=1, intraRefreshPeriod=300, intraRefreshCnt=299`, `outputRecoveryPointSEI=1`.
- **Reference-frame invalidation**: DPB default 5 (8 for AV1), `list0 = NV_ENC_NUM_REF_FRAMES_1`, `enableNonRefP`.
- **Split-frame encoding** on NVENC API 13+ (`NV_ENC_SPLIT_AUTO_MODE`) for 8K.
- Single registered input buffer (synchronous) or async events.

Video transport: RTP header + **Reed-Solomon FEC** (moonlight-common-c): `data_shards = 255*100/(100+fecPercentage)`, parity = round(data * F/100), up to **4 FEC blocks per frame**. **Pacing**: batches ≤64 packets and ≤64 KB, paced at **80% of 1 Gbps** with timer sleeps to per-ms deadlines. Optional per-shard AES-GCM.

## 4. moonlight-android (client)

Reassembly/FEC native (moonlight-common-c: RTP reorder + RS FEC + depacketizer). Java layer = the MediaCodec low-latency reference:
- **Low-latency ladder with fallback** (drop one tier per failure): 1) `"low-latency"=1`; 2) `+ "vdec-lowlatency"=1`; 3) `+ KEY_OPERATING_RATE = Short.MAX_VALUE` else `KEY_PRIORITY = 0`; 4) per-vendor keys: Qualcomm `vendor.qti-ext-dec-picture-order.enable=1` + `vendor.qti-ext-dec-low-latency.enable=1`; Kirin/Exynos/Amlogic variants.
- Base format: `KEY_FRAME_RATE`, `KEY_MAX_WIDTH/HEIGHT`, `KEY_COLOR_RANGE/STANDARD/TRANSFER`, HDR static info CTA-861.3.
- CSD as `BUFFER_FLAG_CODEC_CONFIG` buffer or fused into IDR input buffer.
- Output queue **limited to 2**; `Choreographer.FrameCallback` releases **at most one frame per vsync** when due (`delta >= 800000000/refreshRate` ns).
- Codec-error ladder: flush → restart → reset with 3 s grace.

## 5. What VRStream should copy vs do differently

- **Copy ALVR's NVENC recipe** (most Quest-tuned): LOW_LATENCY tuning, CBR, infinite GOP + manual FORCEIDR, `frameIntervalP=1`, **+ `zeroReorderDelay=1` from Sunshine** (ALVR misses it), VBV ≈ 1.1 frames with full initial delay, `repeatSPSPPS`, spatial AQ, quarter-res multipass, `operating-rate=i32::MAX` on decoder.
- **Do differently: ≥2 NVENC buffers** (`nExtraOutputDelay ≥ 1`) or async completion events so encode and D3D11 copies overlap; ALVR's single-buffer sync encode wastes ~1 ms/frame.
- **Copy Sunshine's slice mode** (`sliceMode=3`) and **reference-frame invalidation** — RFI turns packet loss into a partial glitch instead of a full IDR stall.
- **FEC**: ALVR v21 and WiVRn have none; copy Sunshine's scheme (D = 255·100/(100+F), ≤4 blocks/frame, adaptive percentage). Our Reed-Solomon core already does this.
- **Pace packets properly — the biggest gap in ALVR**: batched pacing (≤64 packets/batch, ≈80% of measured link capacity, per-ms deadlines). ALVR blasts shards back-to-back → bufferbloat and jitter on congested links.
- **Copy ALVR's multiplexed single UDP socket** (shard prefix with stream_id, per-shard locking so tracking interleaves video shards) — but with explicit fixed-width headers, not bincode-of-a-Rust-enum.
- **Add real clock sync** (WiVRn's regression estimator, 3x-mean-RTT outlier rejection). ALVR has none.
- **Copy WiVRn's per-frame feedback timestamps** (11 stages) — direct measurement instead of ALVR's residual subtraction.
- **Keep ALVR's ABR core** but feed it direct measurements; add loss-rate as an input signal (ALVR reacts only via IDR storms).
- **Copy WiVRn's server-driven tracking pattern** (per-device `prediction_ns`, phased sampling) for controllers later.
- **On the client, copy moonlight's low-latency ladder** with retry-configure, plus `vendor.qti-ext-dec-low-latency.enable=1` unconditionally on Quest (ALVR).
- **Use async AMediaCodec callbacks** (WiVRn) into `AImageReader` (surface mode, PRIVATE format, GPU_SAMPLED_IMAGE, maxImages ≈ 10).
- **Copy ALVR's decoder queue policy** (running-average buffering cap, drop-oldest, IDR on starvation); prefer OpenXR frame timing over wall-clock release hacks on Quest.
- **SteamVR driver skeleton** (if targeting SteamVR): `IVRDriverDirectModeComponent`, DXGI shared handles + keyed mutex, pose-history matching (250 ms window). Longer term prefer an OpenXR APILayer path (WiVRn/Monado model).
- **Encrypt eventually** (WiVRn's X25519 + AEAD); ALVR v21 ships plaintext.
- **IDR scheduler with min-interval rate limiting**; "freeze on last good frame until IDR" as a user option.
- **Control channel separate from media** (reliable, length-prefixed, keepalive 500 ms/2 s) while allowing stream multiplexing so haptics never wait behind video.
- **SDR: feed RGBA/ABGR straight into NVENC** (internal RGB→YUV, free); HDR: NV12/P010 via shader conversion like ALVR.
- **Single-frame stereo + fixed foveated encoding** (ALVR model) beats WiVRn's per-eye streams for simplicity.
