// JNI entry points for the VRStream Quest client.
#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "client_net.h"
#include "media_decoder.h"
#include "xr_renderer.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VRStream", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VRStream", __VA_ARGS__)

namespace {

std::atomic<bool> g_running{false};

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
Java_com_vrstream_client_MainActivity_nativeStart(JNIEnv* env, jobject activity,
                                                  jstring host, jint port) {
    if (g_running.exchange(true)) return;  // already running

    const char* hostChars = env->GetStringUTFChars(host, nullptr);
    std::string hostStr(hostChars ? hostChars : "255.255.255.255");
    env->ReleaseStringUTFChars(host, hostChars);

    jobject activityRef = env->NewGlobalRef(activity);
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);

    auto* net = new vrstream::ClientNet({hostStr, static_cast<uint16_t>(port)});
    auto* decoder = new vrstream::VideoDecoder();
    auto* renderer = new vrstream::XrRenderer();

    auto finish = [&] {
        net->stop();
        decoder->shutdown();
        renderer->shutdown();
        delete net;
        delete decoder;
        delete renderer;
        env->DeleteGlobalRef(activityRef);
        g_running = false;
    };

    // Wire: network frames -> decoder; renderer pose -> tracking uplink.
    if (!net->start(
            [decoder](const uint8_t* data, size_t len, uint64_t pts, uint32_t, bool) {
                decoder->feed(data, len, pts);
            },
            [renderer] { return renderer->lastPose(); })) {
        LOGE("could not reach a VRStream host at %s:%u", hostStr.c_str(),
             static_cast<unsigned>(port));
        finish();
        return;
    }

    const auto& neg = net->negotiated();
    uint32_t frameW = neg.eyeWidth * 2;  // both eyes side by side
    uint32_t frameH = neg.eyeHeight;
    vrstream::Codec codec = static_cast<vrstream::Codec>(neg.codec);

    if (!decoder->init(frameW, frameH, codec)) {
        LOGE("decoder init failed (%ux%u %s)", frameW, frameH, vrstream::codecName(codec));
        finish();
        return;
    }

    if (!renderer->init(vm, activityRef, neg.eyeWidth, neg.eyeHeight, neg.refreshRateHz)) {
        LOGE("renderer init failed");
        finish();
        return;
    }

    LOGI("streaming %ux%u %s @ %u Hz", frameW, frameH, vrstream::codecName(codec),
         neg.refreshRateHz);
    renderer->run(g_running, *decoder);
    LOGI("render loop ended");

    finish();
}

extern "C" JNIEXPORT void JNICALL
Java_com_vrstream_client_MainActivity_nativeStop(JNIEnv*, jobject) {
    g_running = false;
}
