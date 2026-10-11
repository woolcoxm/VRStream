// Commits a seated/standing chaperone to the running SteamVR so a fresh
// virtual HMD doesn't block on "first setup" / room setup.
// Uses SteamVR's own openvr_api.dll via the C entry points.
//
// Usage: chaperone_commit.exe [path-to-openvr_api.dll]
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "openvr.h"

typedef uint32_t(VR_CALLTYPE* VRInitInternal2Fn)(vr::EVRInitError*,
                                                 vr::EVRApplicationType, const char*);
typedef void* (VR_CALLTYPE* VRGetGenericInterfaceFn)(const char*,
                                                     vr::EVRInitError*);
typedef void (VR_CALLTYPE* VRShutdownInternalFn)();

int main(int argc, char** argv) {
    const char* dllPath = argc > 1
                              ? argv[1]
                              : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\"
                                "SteamVR\\bin\\win64\\openvr_api.dll";
    HMODULE lib = LoadLibraryA(dllPath);
    if (!lib) {
        std::printf("openvr_api.dll load failed\n");
        return 1;
    }
    auto init = reinterpret_cast<VRInitInternal2Fn>(
        GetProcAddress(lib, "VR_InitInternal2"));
    auto getIface = reinterpret_cast<VRGetGenericInterfaceFn>(
        GetProcAddress(lib, "VR_GetGenericInterface"));
    auto shutdown = reinterpret_cast<VRShutdownInternalFn>(
        GetProcAddress(lib, "VR_ShutdownInternal"));
    if (!init || !getIface || !shutdown) {
        std::printf("exports missing\n");
        return 1;
    }

    vr::EVRInitError err = vr::VRInitError_None;
    init(&err, vr::VRApplication_Other, "vrstream_chaperone_commit");
    if (err != vr::VRInitError_None) {
        std::printf("VR_InitInternal2 error %d (is SteamVR running?)\n", (int)err);
        return 1;
    }

    auto* setup = static_cast<vr::IVRChaperoneSetup*>(
        getIface(vr::IVRChaperoneSetup_Version, &err));
    if (!setup) {
        std::printf("IVRChaperoneSetup unavailable: %d\n", (int)err);
        shutdown();
        return 1;
    }

    const vr::HmdMatrix34_t identity = {
        {{1.0f, 0.0f, 0.0f, 0.0f},
         {0.0f, 1.0f, 0.0f, 0.0f},
         {0.0f, 0.0f, 1.0f, 0.0f}}};
    vr::HmdVector2_t perimeter[4] = {{-1.5f, -1.5f}, {-1.5f, 1.5f},
                                     {1.5f, 1.5f}, {1.5f, -1.5f}};
    setup->SetWorkingPerimeter(perimeter, 4);
    setup->SetWorkingStandingZeroPoseToRawTrackingPose(&identity);
    setup->SetWorkingSeatedZeroPoseToRawTrackingPose(&identity);
    setup->SetWorkingPlayAreaSize(1.5f, 1.5f);
    setup->CommitWorkingCopy(vr::EChaperoneConfigFile_Live);
    shutdown();
    std::printf("chaperone committed (seated+standing identity, 3x3m)\n");
    return 0;
}
