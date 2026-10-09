// VRStream SteamVR driver.
//
// Presents a virtual HMD to SteamVR (IVRDriverDirectModeComponent), receives
// the game's eye textures as DXGI shared textures, composites them side by
// side into an NVENC input texture, encodes, and feeds the Annex-B stream to
// the vrstream_host session over loopback UDP (feed protocol below). The
// host owns pacing/FEC/ABR — this side is capture + encode only.
//
// Feed protocol (driver -> host, loopback UDP):
//   FeedPacketHeader { magic 'VRSF', frameCounter, fragIdx, fragCount,
//                      frameLen, ptsUs } followed by payload bytes.
//
// Bring-up status: compiles; frame path follows the ALVR OvrDirectMode
// pattern (shared handles + keyed mutex). Needs validation against a live
// vrserver (no SteamVR in the dev environment).
#include <d3d11.h>
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "openvr_driver.h"
#include "d3d_helpers.h"
#include "nvenc_encoder.h"
#include "vrstream/common.h"
#include "vrstream/protocol.h"

using namespace vr;

namespace {

constexpr uint16_t kFeedPort = vrstream::kDefaultFeedPort;
constexpr uint32_t kFeedMtu = 1200;

void logLine(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    // vrserver captures OutputDebugString; also console when present.
    OutputDebugStringA("[vrstream-driver] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

bool sendAll(SOCKET s, const uint8_t* data, size_t len, uint32_t frameCounter,
             uint64_t ptsUs) {
    size_t frags = (len + kFeedMtu - 1) / kFeedMtu;
    if (frags > 512) return false;
    for (size_t i = 0; i < frags; i++) {
        size_t off = i * kFeedMtu;
        size_t n = std::min<size_t>(kFeedMtu, len - off);
        std::vector<uint8_t> dg(sizeof(vrstream::FeedPacketHeader) + n);
        vrstream::FeedPacketHeader h{};
        h.magic = vrstream::kFeedMagic;
        h.frameCounter = frameCounter;
        h.fragIdx = static_cast<uint16_t>(i);
        h.fragCount = static_cast<uint16_t>(frags);
        h.frameLen = static_cast<uint32_t>(len);
        h.ptsUs = ptsUs;
        std::memcpy(dg.data(), &h, sizeof(h));
        std::memcpy(dg.data() + sizeof(h), data + off, n);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(kFeedPort);
        to.sin_addr.s_addr = htonl(0x7f000001);
        sendto(s, reinterpret_cast<const char*>(dg.data()), static_cast<int>(dg.size()), 0,
               reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
    return true;
}

// ---------------------------------------------------------------------------

class VrstreamHmd : public ITrackedDeviceServerDriver,
                    public IVRDriverDirectModeComponent {
  public:
    VrstreamHmd() = default;
    virtual ~VrstreamHmd();

    // ITrackedDeviceServerDriver
    EVRInitError Activate(uint32_t objectIndex) override;
    void Deactivate() override;
    void EnterStandby() override {}
    void* GetComponent(const char* name) override;
    void DebugRequest(const char*, char*, uint32_t) override {}
    DriverPose_t GetPose() override;

    // IVRDriverDirectModeComponent
    void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* desc,
                              SwapTextureSet_t* out) override;
    void DestroySwapTextureSet(SharedTextureHandle_t handle) override;
    void DestroyAllSwapTextureSets(uint32_t unPid) override;
    void GetNextSwapTextureSetIndex(SharedTextureHandle_t handles[2],
                                    uint32_t(*indices)[2]) override {
        for (int i = 0; i < 2; i++) (*indices)[i] = (frameCounter_ + 1) % 3;
    }
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
    void Present(SharedTextureHandle_t syncTexture) override;
    void PostPresent(const Throttling_t*) override {}

  private:
    ID3D11Texture2D* openShared(SharedTextureHandle_t handle);
    bool initGpu(uint32_t w, uint32_t h);

    uint32_t objectId_ = 0;
    std::atomic<bool> active_{false};

    vrstream::D3dAdapter adapter_;  // device chosen for NVENC
    ID3D11Texture2D* staging_ = nullptr;  // side-by-side encoder input
    uint32_t eyeW_ = 0, eyeH_ = 0;

    std::mutex txMx_;
    struct SwapSet {
        std::vector<ID3D11Texture2D*> tex;
        uint32_t pid = 0;
    };
    std::map<SharedTextureHandle_t, SwapSet> sets_;  // keyed by first handle
    std::map<SharedTextureHandle_t, ID3D11Texture2D*> opened_;

    ID3D11Texture2D* layerTex_[2] = {nullptr, nullptr};  // latest submitted
    IDXGIKeyedMutex* layerMutex_[2] = {nullptr, nullptr};

    // Render poses/FOVs captured at SubmitLayer (SteamVR's render truth).
    vrstream::ViewInfoLite views_[2]{};
    bool haveViews_ = false;

    static vrstream::ViewInfoLite matrixToView(const vr::HmdMatrix34_t& pose,
                                               const vr::HmdMatrix44_t& proj);

    vrstream::NvencEncoder encoder_;
    SOCKET feedSock_ = INVALID_SOCKET;
    uint32_t frameCounter_ = 0;
};

// ---------------------------------------------------------------------------

static VrstreamHmd* g_hmd = nullptr;
VrstreamHmd::~VrstreamHmd() {
    if (feedSock_ != INVALID_SOCKET) closesocket(feedSock_);
}

EVRInitError VrstreamHmd::Activate(uint32_t objectIndex) {
    objectId_ = objectIndex;
    active_ = true;

    VRProperties()->SetStringProperty(objectIndex, Prop_ModelNumber_String, "vrstream_hmd_1");
    VRProperties()->SetStringProperty(objectIndex, Prop_SerialNumber_String, "vrstream-1");
    VRProperties()->SetStringProperty(objectIndex, Prop_ManufacturerName_String, "VRStream");
    VRProperties()->SetStringProperty(objectIndex, Prop_TrackingFirmwareVersion_String, "1.0");
    VRProperties()->SetStringProperty(objectIndex, Prop_HardwareRevision_String, "1.0");
    VRProperties()->SetBoolProperty(objectIndex, Prop_NeverTracked_Bool, false);
    VRProperties()->SetUint64Property(objectIndex, Prop_CurrentUniverseId_Uint64, 2);
    VRProperties()->SetBoolProperty(objectIndex, Prop_DeviceProvidesBatteryStatus_Bool, false);
    VRProperties()->SetStringProperty(objectIndex, Prop_RenderModelName_String, "generic_hmd");
    VRProperties()->SetBoolProperty(objectIndex, Prop_WillDriftInYaw_Bool, false);
    VRProperties()->SetStringProperty(objectIndex, Prop_RegisteredDeviceType_String,
                                    "vrstream/hmd");
    VRProperties()->SetBoolProperty(objectIndex, Prop_DeviceIsWireless_Bool, true);
    VRProperties()->SetInt32Property(objectIndex, Prop_DeviceClass_Int32,
                                   TrackedDeviceClass_HMD);
    VRProperties()->SetBoolProperty(objectIndex, Prop_DisplayAllowNightMode_Bool, true);
    VRProperties()->SetStringProperty(objectIndex, Prop_TrackingSystemName_String,
                                    "vrstream");

    eyeW_ = 1536;
    eyeH_ = 1600;

    if (!initGpu(eyeW_ * 2, eyeH_)) {
        logLine("GPU init failed; frames will not stream");
        // Still activate so SteamVR can start; sessions report the failure.
    }

    DriverPose_t pose = {0};
    pose.poseIsValid = true;
    pose.result = TrackingResult_Running_OK;
    pose.deviceIsConnected = true;
    pose.qWorldFromDriverRotation = {1, 0, 0, 0};
    pose.qDriverFromHeadRotation = {1, 0, 0, 0};
    VRServerDriverHost()->TrackedDevicePoseUpdated(objectIndex, pose, sizeof(pose));
    logLine("activated (object %u, %ux%u per eye)", objectIndex, eyeW_, eyeH_);
    return VRInitError_None;
}

void VrstreamHmd::Deactivate() {
    active_ = false;
    for (auto& [h, t] : opened_)
        if (t) t->Release();
    opened_.clear();
    if (staging_) {
        staging_->Release();
        staging_ = nullptr;
    }
}

void* VrstreamHmd::GetComponent(const char* name) {
    if (!strcmp(name, IVRDriverDirectModeComponent_Version)) return this;
    return nullptr;
}

DriverPose_t VrstreamHmd::GetPose() {
    DriverPose_t pose = {0};
    pose.poseIsValid = true;
    pose.result = TrackingResult_Running_OK;
    pose.deviceIsConnected = true;
    pose.qWorldFromDriverRotation = {1, 0, 0, 0};
    pose.qDriverFromHeadRotation = {1, 0, 0, 0};
    return pose;
}

bool VrstreamHmd::initGpu(uint32_t w, uint32_t h) {
    auto adapters = vrstream::enumerateD3dAdapters();
    for (auto& a : adapters) {
        vrstream::NvencEncoder::Caps caps;
        if (!vrstream::NvencEncoder::probe(a.device.Get(), &caps)) continue;
        adapter_ = a;
        if (!encoder_.init(a.device.Get(), vrstream::Codec::H264, w, h, 90, 150'000'000))
            continue;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (a.device->CreateTexture2D(&td, nullptr, &staging_) != S_OK) continue;

        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        feedSock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int buf = 2 * 1024 * 1024;
        setsockopt(feedSock_, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof(buf));
        logLine("encoding on %s, feed -> 127.0.0.1:%u", a.name.c_str(), kFeedPort);
        return true;
    }
    return false;
}

ID3D11Texture2D* VrstreamHmd::openShared(SharedTextureHandle_t handle) {
    auto it = opened_.find(handle);
    if (it != opened_.end()) return it->second;
    if (!adapter_.device) return nullptr;
    HANDLE native = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
    ID3D11Texture2D* tex = nullptr;
    if (adapter_.device->OpenSharedResource(native, IID_PPV_ARGS(&tex)) != S_OK) {
        logLine("OpenSharedResource failed for handle %llx", (unsigned long long)handle);
        return nullptr;
    }
    opened_[handle] = tex;
    return tex;
}

void VrstreamHmd::CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* desc,
                                       SwapTextureSet_t* out) {
    if (!adapter_.device) {
        // Create a fallback device on the default adapter so app rendering can
        // proceed even when NVENC init failed.
        auto adapters = vrstream::enumerateD3dAdapters();
        if (!adapters.empty()) adapter_ = adapters.front();
    }
    SwapSet set;
    set.pid = unPid;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = desc->nWidth;
    td.Height = desc->nHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = desc->nSampleCount;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED;

    for (int i = 0; i < 3; i++) {
        ID3D11Texture2D* tex = nullptr;
        if (adapter_.device->CreateTexture2D(&td, nullptr, &tex) != S_OK || !tex) {
            out->rSharedTextureHandles[0] = 0;
            return;
        }
        IDXGIResource* res = nullptr;
        if (tex->QueryInterface(IID_PPV_ARGS(&res)) == S_OK && res) {
            HANDLE h = nullptr;
            res->GetSharedHandle(&h);
            out->rSharedTextureHandles[i] =
                static_cast<SharedTextureHandle_t>(reinterpret_cast<uintptr_t>(h));
            res->Release();
        }
        set.tex.push_back(tex);
    }
    out->unTextureFlags = 0;
    {
        std::lock_guard<std::mutex> lk(txMx_);
        sets_[out->rSharedTextureHandles[0]] = set;
    }
}

void VrstreamHmd::DestroySwapTextureSet(SharedTextureHandle_t handle) {
    std::lock_guard<std::mutex> lk(txMx_);
    auto it = sets_.find(handle);
    if (it == sets_.end()) return;
    for (auto* t : it->second.tex) t->Release();
    sets_.erase(it);
}

void VrstreamHmd::DestroyAllSwapTextureSets(uint32_t unPid) {
    std::lock_guard<std::mutex> lk(txMx_);
    for (auto it = sets_.begin(); it != sets_.end();) {
        if (it->second.pid == unPid) {
            for (auto* t : it->second.tex) t->Release();
            it = sets_.erase(it);
        } else {
            ++it;
        }
    }
}

// OpenVR matrices are column-major: element(row, col) = m[col][row].
vrstream::ViewInfoLite VrstreamHmd::matrixToView(const vr::HmdMatrix34_t& pose,
                                                 const vr::HmdMatrix44_t& proj) {
    vrstream::ViewInfoLite v{};
    v.px = pose.m[3][0];
    v.py = pose.m[3][1];
    v.pz = pose.m[3][2];

    // Rotation 3x3 -> quaternion (Shepperd's method, largest-diagonal branch).
    const float r00 = pose.m[0][0], r10 = pose.m[1][0], r20 = pose.m[2][0];
    const float r01 = pose.m[0][1], r11 = pose.m[1][1], r21 = pose.m[2][1];
    const float r02 = pose.m[0][2], r12 = pose.m[1][2], r22 = pose.m[2][2];
    const float tr = r00 + r11 + r22;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        v.qw = 0.25f * s;
        v.qx = (r21 - r12) / s;
        v.qy = (r02 - r20) / s;
        v.qz = (r12 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        v.qw = (r21 - r12) / s;
        v.qx = 0.25f * s;
        v.qy = (r01 + r10) / s;
        v.qz = (r02 + r20) / s;
    } else if (r11 > r22) {
        float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        v.qw = (r02 - r20) / s;
        v.qx = (r01 + r10) / s;
        v.qy = 0.25f * s;
        v.qz = (r12 + r21) / s;
    } else {
        float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        v.qw = (r12 - r01) / s;
        v.qx = (r02 + r20) / s;
        v.qy = (r12 + r21) / s;
        v.qz = 0.25f * s;
    }

    // FOV from the projection (symmetric approximation from the cotangents;
    // SteamVR's small asymmetric offsets are absorbed by client timewarp).
    const float cotX = proj.m[0][0];
    const float cotY = proj.m[1][1];
    float fx = cotX > 0.0001f ? std::atan(1.0f / cotX) : 1.0f;
    float fy = cotY > 0.0001f ? std::atan(1.0f / cotY) : 1.0f;
    v.fovLeft = -fx;
    v.fovRight = fx;
    v.fovUp = fy;
    v.fovDown = -fy;
    return v;
}

void VrstreamHmd::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
    for (int eye = 0; eye < 2; eye++) {
        views_[eye] = matrixToView(perEye[eye].mHmdPose, perEye[eye].mProjection);
        haveViews_ = true;
        ID3D11Texture2D* tex = openShared(perEye[eye].hTexture);
        if (!tex) continue;
        if (layerMutex_[eye]) {
            layerMutex_[eye]->Release();
            layerMutex_[eye] = nullptr;
        }
        layerTex_[eye] = tex;
        tex->QueryInterface(IID_PPV_ARGS(&layerMutex_[eye]));
    }
}

void VrstreamHmd::Present(SharedTextureHandle_t syncTexture) {
    (void)syncTexture;
    if (!active_ || !staging_ || !encoder_.ok()) return;
    auto* ctx = adapter_.context.Get();

    // Acquire the compositor's textures (keyed mutex, 10 ms budget). On
    // contention we skip the frame rather than stall the pipeline.
    for (int eye = 0; eye < 2; eye++)
        if (layerMutex_[eye] && layerMutex_[eye]->AcquireSync(0, 10) != S_OK) return;

    uint64_t pts = vrstream::nowUs();
    D3D11_BOX left{0, 0, 0, eyeW_, eyeH_, 1};
    D3D11_BOX right{eyeW_, 0, 0, eyeW_ * 2, eyeH_, 1};
    if (layerTex_[0])
        ctx->CopySubresourceRegion(staging_, 0, 0, 0, 0, layerTex_[0], 0, &left);
    if (layerTex_[1])
        ctx->CopySubresourceRegion(staging_, 0, eyeW_, 0, 0, layerTex_[1], 0, &right);

    std::vector<uint8_t> annexB;
    bool ok = encoder_.encodeGpu(staging_, frameCounter_ == 0, pts, annexB);

    for (int eye = 0; eye < 2; eye++)
        if (layerMutex_[eye]) layerMutex_[eye]->ReleaseSync(0);

    if (ok && !annexB.empty()) {
        // Render poses first (small datagram), then the fragmented bitstream.
        if (haveViews_) {
            vrstream::FeedMetaMsg fm{};
            fm.magic = vrstream::kFeedMetaMagic;
            fm.frameCounter = frameCounter_;
            fm.ptsUs = pts;
            fm.meta.views[0] = views_[0];
            fm.meta.views[1] = views_[1];
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(kFeedPort);
            to.sin_addr.s_addr = htonl(0x7f000001);
            sendto(feedSock_, reinterpret_cast<const char*>(&fm), sizeof(fm), 0,
                   reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
        sendAll(feedSock_, annexB.data(), annexB.size(), frameCounter_, pts);
    }
    frameCounter_++;
}

// ---------------------------------------------------------------------------

class SProvider : public IServerTrackedDeviceProvider {
  public:
    EVRInitError Init(IVRDriverContext* context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);  // returns on failure
        if (!g_hmd) g_hmd = new VrstreamHmd();
        bool ok = VRServerDriverHost()->TrackedDeviceAdded(
            "vrstream-1", TrackedDeviceClass_HMD, g_hmd);
        logLine("driver init (hmd added: %d)", ok);
        return ok ? VRInitError_None : VRInitError_Driver_Failed;
    }
    void Cleanup() override {}
    const char* const* GetInterfaceVersions() override {
        static const char* versions[] = {IVRServerDriverHost_Version, nullptr};
        return versions;
    }
    void RunFrame() override {}
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}
};

}  // namespace

static SProvider g_provider;

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* name,
                                                        int* returnCode) {
    if (!strcmp(name, IServerTrackedDeviceProvider_Version)) {
        *returnCode = VRInitError_None;
        return &g_provider;
    }
    *returnCode = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
