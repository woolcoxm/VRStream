# Quest 3 client technology: research digest (October 2026)

Topic: building a low-latency VR streaming CLIENT app for Meta Quest 3 in C++ (NDK) with OpenXR. All claims cited.

## 1. OpenXR on Quest 3 from native Android

Meta supports the standard **Khronos OpenXR Android loader** — no proprietary loader required. The supported integration is the Gradle prefab artifact `org.khronos.openxr:openxr_loader_for_android` (loader version **1.0.34 or higher**; older loaders crash, headset OS must be v62+), which provides `libopenxr_loader.so` for arm64-v8a. Link it from the C++ target and call `xrInitializeLoaderKHR` (obtained via `xrGetInstanceProcAddress` with a null instance) with `XrLoaderInitInfoAndroidKHR` from `XR_KHR_loader_init_android` before anything else, then pass `XrInstanceCreateInfoAndroidKHR` (`XR_KHR_android_instance_create`) into `xrCreateInstance`. [Meta: OpenXR Support for Meta Quest Headsets](https://developers.meta.com/vr/documentation/native/android/mobile-openxr). Reference apps: Khronos `hello_xr` (has a GLES backend) and Meta's native samples.

Manifest requirements: the launcher activity needs `<category android:name="com.oculus.intent.category.VR"/>` in its intent filter (required for Horizon OS to grant exclusive OpenXR display access), `android:screenOrientation="landscape"`, `minSdkVersion 29` / `targetSdkVersion 32`, and `<uses-feature android:name="android.hardware.vr.headtracking" android:required="true" android:version="1"/>`. [Meta: Android Manifest Settings](https://developers.meta.com/vr/documentation/native/android/mobile-native-manifest)

Getting a session rendering with OpenGL ES: create EGL display/config/context yourself, then pass them via `XrGraphicsBindingOpenGLESAndroidKHR` (`XR_KHR_opengl_es_enable`); this is exactly what ALVR does (`SessionCreateInfo::Android { display, config, context }`) [ALVR graphics.rs](https://github.com/alvr-org/ALVR/blob/master/alvr/client_openxr/src/graphics.rs).

## 2. Compositor layers vs world layers for video — the latency-hiding trick

Composition layers are sampled once by the compositor and drawn "pre-distorted" straight to the panel, and critically: **the compositor renders layers at its own rate, "which can be faster, and is never lower, than the frame rate of your application"** — so a submitted layer stays smooth and gets reprojected even when the app misses frames [Meta: Compositor Layers](https://developers.meta.com/vr/documentation/native/android/os-compositor-layers). Up to 16 layers/frame; each costs ~0.1 ms flat plus ~0.6 ms fullscreen per-pixel on Quest 2-class hardware. The compositor applies TimeWarp — reprojecting the frame with the freshest head pose at panel rate, independent of app fps [A VR Frame's Life](https://developers.meta.com/vr/blog/a-vr-frames-life), plus positional timewarp using depth [Meta: The Compositor](https://developers.meta.com/vr/documentation/native/android/os-compositor).

**What ALVR and WiVRn actually use: neither quad layers nor eye layers, but a plain `XrCompositionLayerProjection`** — decoded video is blitted into two per-eye GLES swapchains, and the layer is submitted with (a) `displayTime` = the vsync the frame was rendered for and (b) per-view poses = the poses the PC used when rendering. The runtime then timewarps from that stale pose to the actual head pose at display time — late latching for free [ALVR stream.rs](https://github.com/alvr-org/ALVR/blob/master/alvr/client_openxr/src/stream.rs), [WiVRn stream.cpp](https://github.com/WiVRn/WiVRn/blob/master/client/scenes/stream.cpp).

Relevant extensions: `XR_KHR_android_surface_swapchain` (hand an Android `Surface` straight from the decoder to the runtime), `XR_FB_composition_layer_image_layout` (image orientation flags), `XR_FB_composition_layer_settings` (sharpening/supersampling on the submitted layer; used by ALVR/WiVRn). A quad layer suits a cinema-style screen or lobby UI; **for full-field PCVR video the projection-layer + reprojection approach is what ships in ALVR/WiVRn**.

## 3. MediaCodec low-latency decode on Snapdragon XR2 Gen 2

From NDK C++ use `AMediaCodec` (libmediandk). Configure with a **Surface output** — Surface mode is mandatory for the zero-copy hardware path (ByteBuffer mode involves CPU copies). Two ways to get GL access: (a) Java `SurfaceTexture` with `GL_TEXTURE_EXTERNAL_OES` + `updateTexImage()`; or (b) native `AImageReader` created with GPU usage flags, whose buffers arrive as `AHardwareBuffer` you can wrap as an EGLImage — **this is what both ALVR and WiVRn do** [ALVR android.rs](https://github.com/alvr-org/ALVR/blob/master/alvr/client_core/src/video_decoder/android.rs), [WiVRn android_decoder.cpp](https://github.com/WiVRn/WiVRn/blob/master/client/decoder/android/android_decoder.cpp).

Low-latency MediaFormat keys (moonlight-android is the canonical reference):
- `"low-latency"` = `KEY_LOW_LATENCY` (Android 10+; check `CodecCapabilities.FEATURE_LowLatency`)
- `"vendor.qti-ext-dec-low-latency.enable" = 1` — the Qualcomm key, effective on Snapdragon
- `"vendor.low-latency.enable"`, `"vdec-lowlatency"` (MediaTek), etc.
- `KEY_OPERATING_RATE` = `i32::MAX` and `KEY_PRIORITY` = `0`

[Moonlight MediaCodecHelper.java](https://github.com/moonlight-stream/moonlight-android/blob/master/app/src/main/java/com/limelight/binding/video/MediaCodecHelper.java). ALVR's defaults are exactly `operating-rate=i32::MAX`, `priority=0`, `vendor.qti-ext-dec-low-latency.enable=1` (the plain `low-latency` key is commented out because it crashed some smartphones) [ALVR session settings.rs](https://github.com/alvr-org/ALVR/blob/master/alvr/session/src/settings.rs). Configure per-codec MIMEs: `video/avc` (decoder infers High profile from the SPS in `csd-0`), `video/hevc` (Main10 when the bitstream is profile-2 10-bit), `video/av01`. Timestamp semantics: `AMediaCodec_releaseOutputBufferAtTime(idx, ns)` presents the buffer to the surface at the given timestamp (ALVR passes its own queue-time ns, i.e. render now) [MediaCodec docs](https://developer.android.com/reference/android/media/MediaCodec).

## 4. Quest 3 decoder capabilities

Meta's platform media doc confirms hardware decode of AVC/H.264, HEVC, VP9 and **AV1 "on Quest 3 and above only"**, with documented ceilings of 8192x4096@60 for 2D/180 content [Meta: media requirements](https://developers.meta.com/horizon/documentation/android-apps/media-requirements). Practical streaming guidance: H.264 at constant 400–500 Mbps wireless, HEVC 100–150 Mbps [ALVR wiki](https://github.com/alvr-org/ALVR/wiki/Settings-tutorial). Community consensus: H.264 decodes fastest; HEVC/AV1 give better quality per bit; always query `MediaCodecInfo.CodecCapabilities` on-device. A streaming client uses one decoder session — safe regarding `concurrent-instances` limits.

## 5. Frame timing: xrWaitFrame, vsync, decode→submit

The loop is strictly `xrWaitFrame → xrBeginFrame → xrEndFrame`, single-threaded; `xrWaitFrame` paces the app to the display; each wait must be matched by exactly one begin [Meta: Synchronizing and Submitting Frames](https://developers.meta.com/vr/documentation/native/android/mobile-openxr-frames). `predictedDisplayTime` refers to the **midpoint of the interval during which the frame will be visible** — use it for `xrLocateViews` and echo it into `XrFrameEndInfo.displayTime`; `shouldRender=false` still requires begin/end with no layers [XrFrameState — Khronos](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrFrameState.html). Quest 3 panel rates: 72/80/90/120 Hz [Meta: device optimization comparison](https://developers.meta.com/vr/resources/device-optimization-comparison), selectable via `XR_FB_display_refresh_rate`.

Scheduling decode→submit like ALVR: after `xrWaitFrame` returns, poll the decoder's image queue with a deadline of `frame_interval * ~1.5` (poll every 0.5 ms), then acquire swapchain images, `xrLocateViews` at the frame's target time, blit, release, and submit the projection layer with the **frame's own timestamp** (not the next vsync) as displayTime — clamped so a repeated stale timestamp doesn't persist beyond ~1 s. Submit every panel vsync (re-submit the last image with an updated timestamp when no new frame decoded) so timewarp always has fresh data. ALVR keeps only ~1–3 frames of decoder buffering with a running-average governor because buffering adds latency linearly.

## 6. Tracking data OUT of the headset

There is no raw IMU access on Horizon OS; you get fused poses through OpenXR. The head IMU samples at up to 1000 Hz [Meta: Building a Sensor for Low Latency VR](https://www.meta.com/blog/building-a-sensor-for-low-latency-vr/), but poses are exposed to the app at frame rate — call `xrSyncActions` + `xrLocateSpace`/`xrLocateViews` each frame, and you may run a separate pose-sampling thread at higher rate using predicted future times: ALVR polls tracking on its own thread and predicts the pose at `now + measured pipeline latency` [ALVR wiki: How ALVR works](https://github.com/alvr-org/ALVR/wiki/How-ALVR-works). Package each pose with an `XrTime` timestamp over UDP so the PC can re-predict to its render time.

## 7. Audio

Use **AAudio directly from native code** (or Oboe on top): both ALVR and WiVRn build AAudio streams with `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` — WiVRn requests `AAUDIO_SHARING_MODE_EXCLUSIVE`, 48000 Hz, PCM16, `AAUDIO_INPUT_PRESET_UNPROCESSED` for mic, ~5 ms target buffer and 30–50 ms jitter buffer on output [WiVRn audio.cpp](https://github.com/WiVRn/WiVRn/blob/master/client/audio/android/audio.cpp), [ALVR audio.rs](https://github.com/alvr-org/ALVR/blob/master/alvr/client_core/src/audio.rs). Budget ~40–80 ms of output buffering/offset to align with video. Mic needs `RECORD_AUDIO`.

## 8. Networking from native code

Standard **BSD sockets work fine** from NDK C++ — ALVR and WiVRn both use plain UDP/TCP from native code. The one Android-specific necessity: **acquire a `WifiManager.WifiLock`** so the radio doesn't enter power-save. WiVRn does this via JNI: `WifiManager.createWifiLock(4 /*WIFI_MODE_FULL_LOW_LATENCY*/, name)` (mode 3 `WIFI_MODE_FULL_HIGH_PERF` below API 29) plus a `MulticastLock` for discovery, held for the whole session [WiVRn wifi_lock.cpp](https://github.com/WiVRn/WiVRn/blob/master/client/wifi_lock.cpp). Wi-Fi 6 TWT on the AP is a common cause of periodic micro-stutter. Permissions: `INTERNET`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE`, `CHANGE_WIFI_MULTICAST_STATE`.

## Concrete recipe for the VRStream Quest client

1. Project: NDK r27+, `minSdk 29`, `targetSdk 32`, `abiFilters arm64-v8a`; Gradle `prefab true` + `implementation 'org.khronos.openxr:openxr_loader_for_android:1.0.34'`.
2. Manifest: launcher activity with `com.oculus.intent.category.VR`, `screenOrientation="landscape"`, fullscreen theme, `singleTask`; `<uses-feature android.hardware.vr.headtracking required=true>`; permissions INTERNET / ACCESS_NETWORK_STATE / ACCESS_WIFI_STATE / CHANGE_WIFI_MULTICAST_STATE / RECORD_AUDIO.
3. Loader init: `xrInitializeLoaderKHR` with `XrLoaderInitInfoAndroidKHR{ applicationVM, applicationContext }` before `xrCreateInstance`.
4. Instance: extensions `XR_KHR_opengl_es_enable`, `XR_KHR_android_create_instance`, `XR_FB_display_refresh_rate`, optional `XR_FB_composition_layer_settings`.
5. Session: build EGL + GLES 3.1 context → `XrGraphicsBindingOpenGLESAndroidKHR`; pick format from `xrEnumerateSwapchainFormats`.
6. Refresh rate: `xrRequestDisplayRefreshRateFB(90 or 120)`.
7. Swapchains: two per-eye color swapchains, usage COLOR_ATTACHMENT|SAMPLED, sampleCount 1, size = streamed eye resolution.
8. Network thread: blocking `recvfrom` on UDP (SO_RCVBUF several MB); reassemble frames; feed complete access units to the decoder via a small queue.
9. Wi-Fi lock via JNI (`WIFI_MODE_FULL_LOW_LATENCY`) + MulticastLock; hold for the session.
10. Decoder: `AImageReader_newWithUsage(..., AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | GPU_COLOR_OUTPUT, maxImages 4-6)` with image-available listener; `AMediaCodec_createDecoderByType("video/avc"|"video/hevc"|"video/av01")`; `AMediaFormat`: mime, width/height, `csd-0` from SPS/PPS, `OPERATING_RATE=i32::MAX`, `PRIORITY=0`, `"vendor.qti-ext-dec-low-latency.enable"=1`; `configure(..., imagereader window, ...)`; `start`.
11. Decode loop: dequeueInputBuffer → copy → queueInputBuffer with own ns timestamp; `AMediaCodec_releaseOutputBufferAtTime(idx, ts)` to publish; cap image queue at 1–3 frames.
12. Render loop: `xrWaitFrame` → `xrBeginFrame` → poll decoder queue (deadline ≈ predictedDisplayPeriod × 1.5) → acquire/wait swapchain → `xrLocateViews` at the decoded frame's target time → blit AHardwareBuffer/EGLImage into both eye images → release swapchain.
13. Submit: one `XrCompositionLayerProjection`, two views with the **PC's rendering poses** (frame metadata) and `displayTime` = the frame's original target vsync (clamp stale repeats); re-submit last image with updated time when nothing new decoded.
14. Pose upstream: separate thread ≥ frame rate; `xrLocateSpace` for head + controllers at `now + estimated pipeline latency`; pack into UDP.
15. Audio out: AAudio, LOW_LATENCY, try EXCLUSIVE, 48 kHz stereo, ~5 ms callback; jitter buffer 30–50 ms.
16. Mic: AAudio input, 48 kHz mono PCM16, VOICE_COMMUNICATION preset.
17. Codec defaults: H.264 High for min latency (≤500 Mbps), HEVC Main10 at 100–150 Mbps for quality, AV1 only Quest 3 + modern GPU; request IDR on loss recovery.
18. Telemetry: timestamp packet recv / decode done / submit; report to PC for latency estimation and pose-prediction calibration.
