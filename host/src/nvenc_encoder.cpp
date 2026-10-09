#include "nvenc_encoder.h"

#include <d3d11.h>
#include <windows.h>

#include <cstring>

#include "nvEncodeAPI.h"

namespace vrstream {

struct NvencEncoder::Impl {
    HMODULE dll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST nv{};
    void* encoder = nullptr;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    GUID encodeGuid{};
    Codec codec = Codec::H264;
    uint32_t width = 0, height = 0, fps = 90, bitrateBps = 0;

    static constexpr size_t kInFlight = 3;
    ID3D11Texture2D* inputTex[kInFlight] = {};
    NV_ENC_REGISTER_RESOURCE registered[kInFlight] = {};
    NV_ENC_CREATE_BITSTREAM_BUFFER bitstream[kInFlight] = {};
    size_t nextBuffer = 0;

    NV_ENC_INITIALIZE_PARAMS initParams{};
    NV_ENC_CONFIG encodeConfig{};

    uint32_t pendingBitrateBps = 0;  // applied on next encode (reconfigure)

    void destroy();
};

namespace {

#define NVENC_CALL(impl, expr, fail)                                             \
    do {                                                                         \
        NVENCSTATUS _s = (expr);                                                 \
        if (_s != NV_ENC_SUCCESS) {                                              \
            std::fprintf(stderr, "NVENC error %d at %s\n", static_cast<int>(_s), \
                         #expr);                                                 \
            fail;                                                                \
        }                                                                        \
    } while (0)

GUID codecToGuid(Codec c) {
    switch (c) {
        case Codec::H264: return NV_ENC_CODEC_H264_GUID;
        case Codec::H265:
        case Codec::H26510: return NV_ENC_CODEC_HEVC_GUID;
        case Codec::Av1: return NV_ENC_CODEC_AV1_GUID;
        default: return NV_ENC_CODEC_H264_GUID;
    }
}

}  // namespace

bool NvencEncoder::probe(ID3D11Device* device, Caps* capsOut) {
    HMODULE dll = LoadLibraryA("nvEncodeAPI64.dll");
    if (!dll) {
        std::fprintf(stderr, "probe: nvEncodeAPI64.dll load failed\n");
        return false;
    }

    using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto createInstance =
        reinterpret_cast<CreateInstanceFn>(GetProcAddress(dll, "NvEncodeAPICreateInstance"));
    if (!createInstance) {
        std::fprintf(stderr, "probe: NvEncodeAPICreateInstance missing\n");
        FreeLibrary(dll);
        return false;
    }

    NV_ENCODE_API_FUNCTION_LIST nv{};
    nv.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (createInstance(&nv) != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "probe: createInstance failed\n");
        FreeLibrary(dll);
        return false;
    }

    void* enc = nullptr;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS p{};
    p.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    p.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    p.device = device;
    p.apiVersion = NVENCAPI_VERSION;
    NVENCSTATUS os = nv.nvEncOpenEncodeSessionEx(&p, &enc);
    if (os != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "probe: open session failed: %d\n", static_cast<int>(os));
        FreeLibrary(dll);
        return false;
    }

    Caps caps;
    const GUID guids[3] = {NV_ENC_CODEC_H264_GUID, NV_ENC_CODEC_HEVC_GUID,
                           NV_ENC_CODEC_AV1_GUID};
    bool supported[3] = {};
    uint32_t count = 0;
    if (nv.nvEncGetEncodeGUIDCount(enc, &count) == NV_ENC_SUCCESS && count > 0) {
        std::vector<GUID> list(count);
        uint32_t written = 0;
        if (nv.nvEncGetEncodeGUIDs(enc, list.data(), static_cast<uint32_t>(list.size()),
                                   &written) == NV_ENC_SUCCESS) {
            for (uint32_t g = 0; g < written; g++)
                for (int i = 0; i < 3; i++)
                    if (list[g] == guids[i]) supported[i] = true;
        }
    }
    caps.h264 = supported[0];
    caps.h265 = supported[1];
    caps.av1 = supported[2];

    nv.nvEncDestroyEncoder(enc);
    FreeLibrary(dll);

    if (capsOut) {
        caps.adapterName = "NVENC-capable adapter";
        *capsOut = caps;
    }
    return caps.h264 || caps.h265;
}

NvencEncoder::NvencEncoder() = default;
NvencEncoder::~NvencEncoder() {
    if (impl_) impl_->destroy();
}

void NvencEncoder::Impl::destroy() {
    if (!encoder) return;
    for (size_t i = 0; i < kInFlight; i++) {
        if (registered[i].registeredResource)
            nv.nvEncUnregisterResource(encoder, registered[i].registeredResource);
        if (inputTex[i]) {
            inputTex[i]->Release();
            inputTex[i] = nullptr;
        }
        if (bitstream[i].bitstreamBuffer)
            nv.nvEncDestroyBitstreamBuffer(encoder, bitstream[i].bitstreamBuffer);
    }
    nv.nvEncDestroyEncoder(encoder);
    encoder = nullptr;
    if (dll) {
        FreeLibrary(dll);
        dll = nullptr;
    }
}

bool NvencEncoder::init(ID3D11Device* device, Codec codec, uint32_t width, uint32_t height,
                        uint32_t fps, uint32_t bitrateBps) {
    impl_ = std::make_unique<Impl>();
    Impl* im = impl_.get();
    im->device = device;
    device->GetImmediateContext(&im->ctx);
    im->codec = codec;
    im->width = width;
    im->height = height;
    im->fps = fps;
    im->bitrateBps = bitrateBps;
    im->encodeGuid = codecToGuid(codec);

    im->dll = LoadLibraryA("nvEncodeAPI64.dll");
    if (!im->dll) {
        std::fprintf(stderr, "nvEncodeAPI64.dll not found\n");
        return false;
    }
    using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto createInstance =
        reinterpret_cast<CreateInstanceFn>(GetProcAddress(im->dll, "NvEncodeAPICreateInstance"));
    if (!createInstance) return false;
    im->nv.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (createInstance(&im->nv) != NV_ENC_SUCCESS) return false;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp{};
    sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sp.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sp.device = device;
    sp.apiVersion = NVENCAPI_VERSION;
    if (im->nv.nvEncOpenEncodeSessionEx(&sp, &im->encoder) != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "nvEncOpenEncodeSessionEx failed\n");
        return false;
    }

    // Preset P4 + ultra-low-latency tuning as the base (WiVRn's choice; the
    // P1-vs-P4 latency difference is negligible while quality is better).
    NV_ENC_PRESET_CONFIG presetCfg{};
    presetCfg.version = NV_ENC_PRESET_CONFIG_VER;
    presetCfg.presetCfg.version = NV_ENC_CONFIG_VER;
    NVENCSTATUS ps = im->nv.nvEncGetEncodePresetConfigEx(
        im->encoder, im->encodeGuid, NV_ENC_PRESET_P4_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &presetCfg);
    if (ps != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "nvEncGetEncodePresetConfigEx failed: %d\n", (int)ps);
        return false;
    }

    im->encodeConfig = presetCfg.presetCfg;
    im->encodeConfig.profileGUID =
        (codec == Codec::H264) ? NV_ENC_H264_PROFILE_HIGH_GUID : NV_ENC_HEVC_PROFILE_MAIN_GUID;
    im->encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
    im->encodeConfig.frameIntervalP = 1;
    im->encodeConfig.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    im->encodeConfig.mvPrecision = NV_ENC_MV_PRECISION_QUARTER_PEL;

    auto& rc = im->encodeConfig.rcParams;
    rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    rc.averageBitRate = static_cast<uint32_t>(bitrateBps);
    rc.maxBitRate = static_cast<uint32_t>(bitrateBps);
    rc.vbvBufferSize = static_cast<uint32_t>((double)bitrateBps / fps * 1.1);
    rc.vbvInitialDelay = rc.vbvBufferSize;
    rc.zeroReorderDelay = 1;
    rc.lowDelayKeyFrameScale = 1;
    rc.enableLookahead = 0;
    rc.lookaheadDepth = 0;
    rc.disableIadapt = 0;
    rc.disableBadapt = 0;
    rc.enableAQ = 1;  // spatial AQ

    if (codec == Codec::H264) {
        NV_ENC_CONFIG_H264& h = im->encodeConfig.encodeCodecConfig.h264Config;
        h.repeatSPSPPS = 1;
        h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        h.outputBufferingPeriodSEI = 0;
        h.outputPictureTimingSEI = 0;
        h.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
        // CAVLC: measurably faster decode on mobile decoders (ALVR default).
        h.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
        h.level = 0;  // auto
    } else if (codec == Codec::H265 || codec == Codec::H26510) {
        NV_ENC_CONFIG_HEVC& h = im->encodeConfig.encodeCodecConfig.hevcConfig;
        h.repeatSPSPPS = 1;
        h.outputBufferingPeriodSEI = 0;
        h.outputPictureTimingSEI = 0;
        h.level = 0;
    } else {  // AV1
        NV_ENC_CONFIG_AV1& a = im->encodeConfig.encodeCodecConfig.av1Config;
        a.repeatSeqHdr = 1;
        a.chromaFormatIDC = 1;
    }

    NV_ENC_INITIALIZE_PARAMS& ip = im->initParams;
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = im->encodeGuid;
    ip.encodeWidth = width;
    ip.encodeHeight = height;
    ip.darWidth = width;
    ip.darHeight = height;
    ip.frameRateNum = fps;
    ip.frameRateDen = 1;
    ip.enableEncodeAsync = 0;
    ip.enablePTD = 1;
    ip.enableOutputInVidmem = 0;
    ip.maxEncodeWidth = width;
    ip.maxEncodeHeight = height;
    ip.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    ip.encodeConfig = &im->encodeConfig;

    NVENC_CALL(im, im->nv.nvEncInitializeEncoder(im->encoder, &ip), return false);

    // Input textures: RGBA8, render-target bindable, registered with NVENC.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    td.CPUAccessFlags = 0;

    for (size_t i = 0; i < Impl::kInFlight; i++) {
        if (im->device->CreateTexture2D(&td, nullptr, &im->inputTex[i]) != S_OK)
            return false;

        NV_ENC_REGISTER_RESOURCE& rr = im->registered[i];
        rr.version = NV_ENC_REGISTER_RESOURCE_VER;
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        rr.width = width;
        rr.height = height;
        rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR;
        rr.bufferUsage = NV_ENC_INPUT_IMAGE;
        rr.resourceToRegister = im->inputTex[i];
        NVENC_CALL(im, im->nv.nvEncRegisterResource(im->encoder, &rr), return false);

        NV_ENC_CREATE_BITSTREAM_BUFFER& bs = im->bitstream[i];
        bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        NVENC_CALL(im, im->nv.nvEncCreateBitstreamBuffer(im->encoder, &bs), return false);
    }
    return true;
}

bool NvencEncoder::encode(const uint8_t* srcRgba, size_t rowPitch, bool forceIdr,
                          uint64_t ptsUs, std::vector<uint8_t>& outAnnexB) {
    Impl* im = impl_.get();
    if (!im || !im->encoder) return false;

    // Deferred reconfigure (rate-limited by the caller via setBitrate).
    if (im->pendingBitrateBps && im->pendingBitrateBps != im->bitrateBps) {
        im->bitrateBps = im->pendingBitrateBps;
        im->encodeConfig.rcParams.averageBitRate = im->bitrateBps;
        im->encodeConfig.rcParams.maxBitRate = im->bitrateBps;
        im->encodeConfig.rcParams.vbvBufferSize =
            static_cast<uint32_t>((double)im->bitrateBps / im->fps * 1.1);
        im->encodeConfig.rcParams.vbvInitialDelay =
            im->encodeConfig.rcParams.vbvBufferSize;
        NV_ENC_RECONFIGURE_PARAMS rp{};
        rp.version = NV_ENC_RECONFIGURE_PARAMS_VER;
        rp.reInitEncodeParams = im->initParams;
        rp.reInitEncodeParams.encodeConfig = &im->encodeConfig;
        im->nv.nvEncReconfigureEncoder(im->encoder, &rp);
        im->pendingBitrateBps = 0;
    }

    const size_t idx = im->nextBuffer;
    im->nextBuffer = (im->nextBuffer + 1) % Impl::kInFlight;

    // CPU rows -> GPU texture via a staging upload (UpdateSubresource).
    D3D11_BOX box{0, 0, 0, im->width, im->height, 1};
    im->ctx->UpdateSubresource(im->inputTex[idx], 0, &box, srcRgba,
                               static_cast<UINT>(rowPitch), 0);

    NV_ENC_MAP_INPUT_RESOURCE mapped{};
    mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapped.registeredResource = im->registered[idx].registeredResource;
    NVENC_CALL(im, im->nv.nvEncMapInputResource(im->encoder, &mapped), return false);

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = im->width;
    pic.inputHeight = im->height;
    pic.inputPitch = im->width;
    pic.inputTimeStamp = ptsUs;
    pic.inputDuration = 0;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputBuffer = mapped.mappedResource;
    pic.bufferFmt = mapped.mappedBufferFmt;
    pic.outputBitstream = im->bitstream[idx].bitstreamBuffer;
    if (forceIdr) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;

    NVENCSTATUS st = im->nv.nvEncEncodePicture(im->encoder, &pic);
    if (st != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "nvEncEncodePicture failed: %d\n", (int)st);
        im->nv.nvEncUnmapInputResource(im->encoder, mapped.mappedResource);
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = im->bitstream[idx].bitstreamBuffer;
    lock.doNotWait = 0;
    bool ok = true;
    NVENC_CALL(im, im->nv.nvEncLockBitstream(im->encoder, &lock), ok = false);
    if (ok) {
        const uint8_t* p = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
        outAnnexB.insert(outAnnexB.end(), p, p + lock.bitstreamSizeInBytes);
        im->nv.nvEncUnlockBitstream(im->encoder, lock.outputBitstream);
    }
    im->nv.nvEncUnmapInputResource(im->encoder, mapped.mappedResource);
    return ok;
}

bool NvencEncoder::encodeGpu(ID3D11Texture2D* src, bool forceIdr, uint64_t ptsUs,
                             std::vector<uint8_t>& outAnnexB) {
    Impl* im = impl_.get();
    if (!im || !im->encoder || !src) return false;

    if (im->pendingBitrateBps && im->pendingBitrateBps != im->bitrateBps) {
        im->bitrateBps = im->pendingBitrateBps;
        im->encodeConfig.rcParams.averageBitRate = im->bitrateBps;
        im->encodeConfig.rcParams.maxBitRate = im->bitrateBps;
        im->encodeConfig.rcParams.vbvBufferSize =
            static_cast<uint32_t>((double)im->bitrateBps / im->fps * 1.1);
        im->encodeConfig.rcParams.vbvInitialDelay =
            im->encodeConfig.rcParams.vbvBufferSize;
        NV_ENC_RECONFIGURE_PARAMS rp{};
        rp.version = NV_ENC_RECONFIGURE_PARAMS_VER;
        rp.reInitEncodeParams = im->initParams;
        rp.reInitEncodeParams.encodeConfig = &im->encodeConfig;
        im->nv.nvEncReconfigureEncoder(im->encoder, &rp);
        im->pendingBitrateBps = 0;
    }

    const size_t idx = im->nextBuffer;
    im->nextBuffer = (im->nextBuffer + 1) % Impl::kInFlight;
    im->ctx->CopyResource(im->inputTex[idx], src);

    NV_ENC_MAP_INPUT_RESOURCE mapped{};
    mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapped.registeredResource = im->registered[idx].registeredResource;
    NVENC_CALL(im, im->nv.nvEncMapInputResource(im->encoder, &mapped), return false);

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = im->width;
    pic.inputHeight = im->height;
    pic.inputPitch = im->width;
    pic.inputTimeStamp = ptsUs;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputBuffer = mapped.mappedResource;
    pic.bufferFmt = mapped.mappedBufferFmt;
    pic.outputBitstream = im->bitstream[idx].bitstreamBuffer;
    if (forceIdr) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;

    NVENCSTATUS st = im->nv.nvEncEncodePicture(im->encoder, &pic);
    if (st != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "nvEncEncodePicture failed: %d\n", (int)st);
        im->nv.nvEncUnmapInputResource(im->encoder, mapped.mappedResource);
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = im->bitstream[idx].bitstreamBuffer;
    lock.doNotWait = 0;
    bool ok = true;
    NVENC_CALL(im, im->nv.nvEncLockBitstream(im->encoder, &lock), ok = false);
    if (ok) {
        const uint8_t* p = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
        outAnnexB.insert(outAnnexB.end(), p, p + lock.bitstreamSizeInBytes);
        im->nv.nvEncUnlockBitstream(im->encoder, lock.outputBitstream);
    }
    im->nv.nvEncUnmapInputResource(im->encoder, mapped.mappedResource);
    return ok;
}

void NvencEncoder::setBitrate(uint32_t bps) {
    if (impl_) impl_->pendingBitrateBps = bps;
}

uint32_t NvencEncoder::bitrate() const { return impl_ ? impl_->bitrateBps : 0; }
Codec NvencEncoder::codec() const { return impl_ ? impl_->codec : Codec::H264; }
bool NvencEncoder::ok() const { return impl_ && impl_->encoder; }

}  // namespace vrstream
