#include "media_decoder.h"

#include <android/log.h>
#include <android/hardware_buffer.h>

#include <chrono>
#include <cstring>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VRStream", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VRStream", __VA_ARGS__)

namespace vrstream {

VideoDecoder::~VideoDecoder() { shutdown(); }

const char* mimeFor(Codec c) {
    switch (c) {
        case Codec::H264: return "video/avc";
        case Codec::H265:
        case Codec::H26510: return "video/hevc";
        case Codec::Av1: return "video/av01";
        default: return "video/avc";
    }
}

bool VideoDecoder::init(uint32_t sizeW, uint32_t sizeH, Codec codec, uint32_t maxImages) {
    frameW_ = sizeW;
    frameH_ = sizeH;
    maxImages_ = maxImages;

    media_status_t st = AImageReader_newWithUsage(
        sizeW, sizeH, AIMAGE_FORMAT_PRIVATE,
        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
        maxImages + 2, &reader_);
    if (st != AMEDIA_OK || !reader_) {
        LOGE("AImageReader_newWithUsage failed: %d", (int)st);
        return false;
    }
    AImageReader_ImageListener listener{};
    listener.context = this;
    listener.onImageAvailable = &onImageStatic;
    AImageReader_setImageListener(reader_, &listener);

    if (AImageReader_getWindow(reader_, &window_) != AMEDIA_OK || !window_) {
        LOGE("AImageReader_getWindow failed");
        return false;
    }

    codec_ = AMediaCodec_createDecoderByType(mimeFor(codec));
    if (!codec_) {
        LOGE("AMediaCodec_createDecoderByType(%s) failed", mimeFor(codec));
        return false;
    }

    // Low-latency ladder: full attempt, then retry without the vendor key
    // (moonlight-style; some builds reject unknown vendor keys).
    for (int attempt = 0; attempt < 2; attempt++) {
        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mimeFor(codec));
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_WIDTH, static_cast<int32_t>(sizeW));
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_HEIGHT, static_cast<int32_t>(sizeH));
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, static_cast<int32_t>(sizeW));
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, static_cast<int32_t>(sizeH));
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_OPERATING_RATE, INT32_MAX);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_PRIORITY, 0);
        if (attempt == 0) {
            AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-low-latency.enable", 1);
            AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);
        }

        AMediaCodecOnAsyncNotifyCallback cb{};
        cb.onAsyncInputAvailable = &onInputStatic;
        cb.onAsyncOutputAvailable = &onOutputStatic;
        cb.onAsyncFormatChanged = nullptr;
        cb.onAsyncError = &onErrorStatic;

        st = AMediaCodec_configure(codec_, fmt, window_, nullptr, 0);
        AMediaFormat_delete(fmt);
        if (st != AMEDIA_OK) {
            LOGE("AMediaCodec_configure failed (attempt %d): %d", attempt, (int)st);
            continue;
        }
        st = AMediaCodec_setAsyncNotifyCallback(codec_, cb, this);
        if (st != AMEDIA_OK) {
            LOGE("setAsyncNotifyCallback failed: %d", (int)st);
            continue;
        }
        st = AMediaCodec_start(codec_);
        if (st == AMEDIA_OK) {
            ok_ = true;
            LOGI("decoder ready: %s %ux%u (vendor-key=%d)", mimeFor(codec), sizeW, sizeH,
                 attempt == 0);
            return true;
        }
        LOGE("AMediaCodec_start failed: %d", (int)st);
    }
    return false;
}

void VideoDecoder::shutdown() {
    ok_ = false;
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    std::lock_guard<std::mutex> lk(mx_);
    while (!decoded_.empty()) {
        AImage_delete(decoded_.front());
        decoded_.pop_front();
    }
    if (reader_) {
        AImageReader_delete(reader_);
        reader_ = nullptr;
    }
    window_ = nullptr;
}

void VideoDecoder::feed(const uint8_t* data, size_t len, uint64_t ptsUs) {
    if (!ok_ || !codec_) return;

    int32_t idx = -1;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (freeInputs_.empty()) return;  // drop frame rather than add latency
        idx = freeInputs_.front();
        freeInputs_.pop_front();
    }

    size_t cap = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec_, idx, &cap);
    if (!buf || cap < len) {
        AMediaCodec_queueInputBuffer(codec_, idx, 0, 0, ptsUs * 1000, 0);  // return buffer
        return;
    }
    std::memcpy(buf, data, len);
    media_status_t st = AMediaCodec_queueInputBuffer(codec_, idx, 0, len, ptsUs * 1000, 0);
    if (st != AMEDIA_OK) LOGE("queueInputBuffer: %d", (int)st);
}

void VideoDecoder::onInputStatic(AMediaCodec*, void* self, int32_t index) {
    static_cast<VideoDecoder*>(self)->drainInput(index);
}

void VideoDecoder::drainInput(int32_t index) {
    std::lock_guard<std::mutex> lk(mx_);
    freeInputs_.push_back(index);
}

void VideoDecoder::onOutputStatic(AMediaCodec*, void* self, int32_t index,
                                  AMediaCodecBufferInfo* info) {
    static_cast<VideoDecoder*>(self)->handleOutput(index, info);
}

void VideoDecoder::handleOutput(int32_t index, AMediaCodecBufferInfo* info) {
    (void)info;
    if (!codec_) return;
    // Surface mode: rendering publishes the buffer to the AImageReader.
    media_status_t st = AMediaCodec_releaseOutputBuffer(codec_, index, true);
    if (st != AMEDIA_OK) LOGE("releaseOutputBuffer: %d", (int)st);
    framesDecoded_++;
}

void VideoDecoder::onErrorStatic(AMediaCodec*, void* self, media_status_t err,
                                 int32_t actionCode, const char* detail) {
    LOGE("MediaCodec error %d action=%d %s", (int)err, actionCode, detail ? detail : "");
}

void VideoDecoder::onImageStatic(void* self, AImageReader* reader) {
    auto* dec = static_cast<VideoDecoder*>(self);
    AImage* img = nullptr;
    if (AImageReader_acquireLatestImage(reader, &img) != AMEDIA_OK || !img) return;

    int32_t w = 0, h = 0;
    AImage_getWidth(img, &w);
    AImage_getHeight(img, &h);

    {
        std::lock_guard<std::mutex> lk(dec->mx_);
        // Bounded queue: drop the oldest (latency never accumulates).
        while (dec->decoded_.size() >= dec->maxImages_) {
            AImage_delete(dec->decoded_.front());
            dec->decoded_.pop_front();
        }
        dec->decoded_.push_back(img);
    }
}

bool VideoDecoder::latest(DecodedFrame& out) {
    std::lock_guard<std::mutex> lk(mx_);
    if (decoded_.empty()) return false;
    AImage* img = decoded_.back();
    decoded_.pop_back();  // ownership transfer
    int32_t w = 0, h = 0;
    AImage_getWidth(img, &w);
    AImage_getHeight(img, &h);
    int64_t ts = 0;
    AImage_getTimestamp(img, &ts);
    out.image = img;
    out.width = w;
    out.height = h;
    out.ptsUs = static_cast<uint64_t>(ts / 1000);  // ns -> us
    return true;
}

void VideoDecoder::release(DecodedFrame& f) {
    if (f.image) AImage_delete(f.image);
    f.image = nullptr;
}

}  // namespace vrstream
