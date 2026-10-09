#include "xr_renderer.h"

#include <android/log.h>
#include <android/native_window.h>
#include <media/NdkImage.h>
#include <openxr/openxr_platform.h>

#include <cstring>
#include <vector>

#include "vrstream/common.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VRStream", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VRStream", __VA_ARGS__)

#define XR_CHECK(expr, fail)                                          \
    do {                                                              \
        XrResult _r = (expr);                                         \
        if (_r != XR_SUCCESS) {                                       \
            LOGE("XR error %d at %s", (int)_r, #expr);                \
            fail;                                                     \
        }                                                             \
    } while (0)

namespace vrstream {

namespace {

const char* kVertexShader = R"glsl(#version 300 es
layout(location = 0) in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)glsl";

// The video frame holds both eyes side by side; each eye samples its half.
const char* kFragmentShader = R"glsl(#version 300 es
#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES uVideo;
uniform lowp int uEye;
in vec2 vUv;
out vec4 oColor;
void main() {
    vec2 uv = vec2((float(uEye) + vUv.x) * 0.5, 1.0 - vUv.y);
    oColor = vec4(texture(uVideo, uv).rgb, 1.0);
}
)glsl";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]{};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("shader compile: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

bool XrRenderer::init(void* javaVm, void* androidContext, uint32_t eyeW, uint32_t eyeH,
                      uint32_t refreshHz) {
    javaVm_ = javaVm;
    androidContext_ = androidContext;
    return initEgl() && initOpenXr(eyeW, eyeH, refreshHz) && initGl();
}

bool XrRenderer::initEgl() {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) return false;
    if (!eglInitialize(display_, nullptr, nullptr)) return false;

    const EGLint cfgAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                               EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(display_, cfgAttrs, &config_, 1, &n) || n < 1) {
        LOGE("eglChooseConfig failed");
        return false;
    }
    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctxAttrs);
    if (context_ == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return false;
    }
    // Surfaceless current context (EGL_KHR_surfaceless_context on Quest);
    // OpenXR owns the actual swapchains.
    if (!eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_)) {
        LOGE("surfaceless eglMakeCurrent failed");
        return false;
    }
    return true;
}

bool XrRenderer::initOpenXr(uint32_t streamW, uint32_t streamH, uint32_t refreshHz) {
    // Android loader init must precede xrCreateInstance.
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          (PFN_xrVoidFunction*)&xrInitializeLoaderKHR);
    if (xrInitializeLoaderKHR) {
        XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = javaVm_;
        li.applicationContext = androidContext_;
        XrResult r = xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR*)&li);
        if (r != XR_SUCCESS) {
            LOGE("xrInitializeLoaderKHR: %d", (int)r);
            return false;
        }
    } else {
        LOGI("xrInitializeLoaderKHR unavailable; relying on implicit loader");
    }

    const char* exts[] = {"XR_KHR_opengl_es_enable", "XR_KHR_android_create_instance",
                          "XR_FB_display_refresh_rate"};
    XrInstanceCreateInfoAndroidKHR ai{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ai.applicationVM = javaVm_;
    ai.applicationActivity = androidContext_;

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ai;
    std::snprintf(ci.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE,
                  "VRStream");
    ci.applicationInfo.applicationVersion = 1;
    std::snprintf(ci.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "vrstream");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount = 3;
    ci.enabledExtensionNames = exts;

    XR_CHECK(xrCreateInstance(&ci, &instance_), return false);
    if (instance_ == XR_NULL_HANDLE) return false;

    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance_, &sg, &systemId), return false);

    XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = display_;
    gb.config = config_;
    gb.context = context_;
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
    si.next = &gb;
    si.systemId = systemId;
    XR_CHECK(xrCreateSession(instance_, &si, &session_), return false);

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    XR_CHECK(xrCreateReferenceSpace(session_, &rs, &viewSpace_), return false);

    PFN_xrRequestDisplayRefreshRateFB requestRate = nullptr;
    xrGetInstanceProcAddr(instance_, "xrRequestDisplayRefreshRateFB",
                          (PFN_xrVoidFunction*)&requestRate);
    if (requestRate) requestRate(session_, static_cast<float>(refreshHz));

    uint32_t viewCount = 0;
    XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                        {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    XR_CHECK(xrEnumerateViewConfigurationViews(instance_, systemId,
                                               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                               &viewCount, views),
             return false);
    eyeW_ = views[0].recommendedImageRectWidth;
    eyeH_ = views[0].recommendedImageRectHeight;
    LOGI("eye swapchains %ux%u (stream %ux%u)", eyeW_, eyeH_, streamW, streamH);

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(session_, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(session_, fmtCount, &fmtCount, fmts.data());
    swapchainFormat_ = GL_RGB565;
    for (auto f : fmts)
        if (f == GL_RGBA8) swapchainFormat_ = GL_RGBA8;

    for (int i = 0; i < 2; i++) {
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags =
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = swapchainFormat_;
        sc.sampleCount = 1;
        sc.width = views[i].recommendedImageRectWidth;
        sc.height = views[i].recommendedImageRectHeight;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        XR_CHECK(xrCreateSwapchain(session_, &sc, &swapchains_[i]), return false);

        uint32_t len = 0;
        xrEnumerateSwapchainImages(swapchains_[i], 0, &len, nullptr);
        swapchainImages_[i].resize(len, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(
                     swapchains_[i], len, &len,
                     (XrSwapchainImageBaseHeader*)swapchainImages_[i].data()),
                 return false);
    }
    return true;
}

bool XrRenderer::initGl() {
    GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) return false;
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]{};
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        LOGE("program link: %s", log);
        return false;
    }
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "uVideo"), 0);
    eyeUniform_ = glGetUniformLocation(program_, "uEye");

    glGenTextures(1, &extTex_);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, extTex_);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glGenFramebuffers(2, eyeFbo_);
    return true;
}

bool XrRenderer::importHardwareBuffer(AHardwareBuffer* buf) {
    if (buf == curBuffer_) return curImage_ != nullptr;
    if (curImage_) eglDestroyImageKHR(display_, curImage_);
    curImage_ = nullptr;
    curBuffer_ = buf;
    if (!buf) return false;

    const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    curImage_ = eglCreateImageKHR(display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                  (EGLClientBuffer)buf, attrs);
    if (!curImage_) {
        LOGE("eglCreateImageKHR failed");
        return false;
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, extTex_);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, curImage_);
    return true;
}

void XrRenderer::run(std::atomic<bool>& running, VideoDecoder& decoder) {
    while (running.load()) {
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XR_CHECK(xrWaitFrame(session_, nullptr, &fs), break);
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(session_, &bi);

        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;

        if (fs.shouldRender && renderIntoSwapchains(fs.predictedDisplayTime, decoder)) {
            XrCompositionLayerProjectionView pv[2] = {
                {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
            for (int eye = 0; eye < 2; eye++) {
                pv[eye].pose = renderPoses_[eye];
                pv[eye].fov = renderFovs_[eye];
                pv[eye].subImage.swapchain = swapchains_[eye];
                pv[eye].subImage.imageRect.offset = {0, 0};
                pv[eye].subImage.imageRect.extent = {(int32_t)eyeW_, (int32_t)eyeH_};
            }
            layer_.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
            layer_.next = nullptr;
            layer_.space = viewSpace_;
            layer_.viewCount = 2;
            layer_.views = pv;
            ei.layerCount = 1;
            ei.layers = (const XrCompositionLayerBaseHeader* const*)&layer_;
        } else {
            ei.layerCount = 0;
            ei.layers = nullptr;
        }
        xrEndFrame(session_, &ei);
        framesRendered_++;
    }
    LOGI("render loop exit after %llu frames", (unsigned long long)framesRendered_);
}

bool XrRenderer::renderIntoSwapchains(XrTime predictedDisplayTime, VideoDecoder& decoder) {
    uint32_t viewCount = 0;
    XrViewLocateInfo lv{XR_TYPE_VIEW_LOCATE_INFO};
    lv.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    lv.displayTime = predictedDisplayTime;
    lv.space = viewSpace_;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    if (xrLocateViews(session_, &lv, &vs, 2, &viewCount, views) != XR_SUCCESS ||
        viewCount != 2)
        return false;

    for (int eye = 0; eye < 2; eye++) {
        renderPoses_[eye] = views[eye].pose;
        renderFovs_[eye] = views[eye].fov;
    }
    {
        std::lock_guard<std::mutex> lk(poseMx_);
        lastPose_.clientTimeUs = nowUs();
        lastPose_.px = views[0].pose.position.x;
        lastPose_.py = views[0].pose.position.y;
        lastPose_.pz = views[0].pose.position.z;
        lastPose_.qx = views[0].pose.orientation.x;
        lastPose_.qy = views[0].pose.orientation.y;
        lastPose_.qz = views[0].pose.orientation.z;
        lastPose_.qw = views[0].pose.orientation.w;
    }

    // Timewarp contract: when the host supplied the poses SteamVR rendered
    // with, submit those instead of freshly located LOCAL poses — the
    // compositor then reprojects from the true render pose to the current
    // head pose. Falls back to LOCAL when the meta packet was lost.
    // (Approximate pairing: newest meta with newest decoded frame; exact
    // frame-index matching lands with decoder queue plumbing.)
    VideoMetaMsg meta;
    if (decoder.takeFrameMeta(meta)) {
        for (int eye = 0; eye < 2; eye++) {
            const auto& v = meta.views[eye];
            renderPoses_[eye].position = {v.px, v.py, v.pz};
            renderPoses_[eye].orientation = {v.qx, v.qy, v.qz, v.qw};
            renderFovs_[eye].angleLeft = v.fovLeft;
            renderFovs_[eye].angleRight = v.fovRight;
            renderFovs_[eye].angleUp = v.fovUp;
            renderFovs_[eye].angleDown = v.fovDown;
        }
    }

    VideoDecoder::DecodedFrame df;
    if (decoder.latest(df)) {
        AHardwareBuffer* buf = nullptr;
        if (df.image && AImage_getHardwareBuffer(df.image, &buf) == AMEDIA_OK && buf) {
            importHardwareBuffer(buf);
            videoW_ = df.width;
            videoH_ = df.height;
        }
        decoder.release(df);
    }

    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, extTex_);
    glUniform1i(eyeUniform_, 0);
    glViewport(0, 0, eyeW_, eyeH_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindVertexArray(vao_);

    for (int eye = 0; eye < 2; eye++) {
        uint32_t idx = 0;
        if (xrAcquireSwapchainImage(swapchains_[eye], nullptr, &idx) != XR_SUCCESS)
            continue;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100 * 1000 * 1000;  // 100 ms; drop frame on longer stall
        if (xrWaitSwapchainImage(swapchains_[eye], &wi) == XR_SUCCESS) {
            glBindFramebuffer(GL_FRAMEBUFFER, eyeFbo_[eye]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   swapchainImages_[eye][idx].image, 0);
            glUniform1i(eyeUniform_, eye);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchains_[eye], &ri);
    }
    return true;
}

void XrRenderer::shutdown() {
    if (program_) glDeleteProgram(program_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (extTex_) glDeleteTextures(1, &extTex_);
    if (eyeFbo_[0]) glDeleteFramebuffers(2, eyeFbo_);
    if (curImage_) eglDestroyImageKHR(display_, curImage_);
    for (int i = 0; i < 2; i++)
        if (swapchains_[i]) xrDestroySwapchain(swapchains_[i]);
    if (viewSpace_) xrDestroySpace(viewSpace_);
    if (session_) xrDestroySession(session_);
    if (instance_) xrDestroyInstance(instance_);
    if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
    if (display_ != EGL_NO_DISPLAY) eglTerminate(display_);
}

}  // namespace vrstream
