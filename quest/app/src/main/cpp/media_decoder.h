// Hardware video decoder: async AMediaCodec (NDK) in surface mode, output
// into an AImageReader so decoded frames arrive as AHardwareBuffers the GL
// renderer can import as EGLImages. Low-latency ladder from the moonlight /
// ALVR study: operating-rate=i32::MAX, priority=0, Qualcomm low-latency
// vendor key (with graceful retry without it).
#pragma once

#include <media/NdkMediaCodec.h>
#include <media/NdkImageReader.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "vrstream/protocol.h"

namespace vrstream {

class VideoDecoder {
  public:
    struct DecodedFrame {
        AImage* image = nullptr;          // caller must return via release()
        uint64_t ptsUs = 0;               // host-clock capture timestamp
        int32_t width = 0, height = 0;
    };

    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // `sizeW/sizeH` frame layout (both eyes side by side), codec from the
    // handshake. `maxImages` bounds decoder->GL queue depth.
    bool init(uint32_t sizeW, uint32_t sizeH, Codec codec, uint32_t maxImages = 6);
    void shutdown();

    // Queue one encoded frame (Annex-B). Thread-safe.
    void feed(const uint8_t* data, size_t len, uint64_t ptsUs);

    // Takes the newest decoded frame out of the queue (ownership transfer);
    // caller returns it via release() when done sampling it.
    bool latest(DecodedFrame& out);
    void release(DecodedFrame& f);

    uint32_t backlog() const {
        std::lock_guard<std::mutex> lk(mx_);
        return static_cast<uint32_t>(decoded_.size());
    }
    uint32_t framesDecoded() const { return framesDecoded_.load(); }

  private:
    static void onInputStatic(AMediaCodec*, void* self, int32_t index);
    static void onOutputStatic(AMediaCodec*, void* self, int32_t index,
                               AMediaCodecBufferInfo* info);
    static void onErrorStatic(AMediaCodec*, void* self, media_status_t err, int32_t actionCode,
                              const char* detail);
    static void onImageStatic(void* self, AImageReader* reader);

    void drainInput(int32_t codecIndex);
    void handleOutput(int32_t codecIndex, AMediaCodecBufferInfo* info);

    AMediaCodec* codec_ = nullptr;
    AImageReader* reader_ = nullptr;
    ANativeWindow* window_ = nullptr;

    mutable std::mutex mx_;
    std::deque<int32_t> freeInputs_;  // codec buffer indices
    std::deque<AImage*> decoded_;     // newest at back; queue owns lifetime
    uint32_t maxImages_ = 6;
    uint32_t frameW_ = 0, frameH_ = 0;
    std::atomic<bool> ok_{false};
    std::atomic<uint32_t> framesDecoded_{0};
};

}  // namespace vrstream
