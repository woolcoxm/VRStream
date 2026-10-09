// OpenXR renderer: GLES session on Quest, blits decoded stereo frames into
// per-eye swapchains and submits a projection layer (the compositor timewarps
// it with the latest head pose — our main latency-hiding mechanism).
#pragma once

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "media_decoder.h"

namespace vrstream {

class XrRenderer {
  public:
    bool init(void* javaVm, void* androidContext, uint32_t eyeW, uint32_t eyeH,
              uint32_t refreshHz);
    void run(std::atomic<bool>& running, VideoDecoder& decoder);
    void shutdown();

    // Latest head pose in OpenXR LOCAL space (for the tracking uplink).
    PoseSample lastPose() {
        std::lock_guard<std::mutex> lk(poseMx_);
        return lastPose_;
    }

  private:
    bool initEgl();
    bool initOpenXr(uint32_t streamW, uint32_t streamH, uint32_t refreshHz);
    bool initGl();
    bool renderIntoSwapchains(XrTime predictedDisplayTime, VideoDecoder& decoder);
    bool importHardwareBuffer(AHardwareBuffer* buf);

    void* javaVm_ = nullptr;
    void* androidContext_ = nullptr;
    uint32_t eyeW_ = 0, eyeH_ = 0;

    // EGL
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;

    // OpenXR
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrSwapchain swapchains_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<XrSwapchainImageOpenGLESKHR> swapchainImages_[2];
    int64_t swapchainFormat_ = 0;

    // GL blit pipeline
    GLuint program_ = 0;
    GLuint vao_ = 0, vbo_ = 0;
    GLuint extTex_ = 0;
    EGLImageKHR curImage_ = nullptr;
    AHardwareBuffer* curBuffer_ = nullptr;
    GLuint eyeFbo_[2] = {0, 0};
    GLint eyeUniform_ = -1;
    int32_t videoW_ = 0, videoH_ = 0;

    // Per-frame render state (poses double as the submitted layer poses in v1).
    XrPosef renderPoses_[2]{};
    XrFovf renderFovs_[2]{};
    XrCompositionLayerProjection layer_{};

    std::mutex poseMx_;
    PoseSample lastPose_{};
    uint64_t framesRendered_ = 0;
};

}  // namespace vrstream
