#include "nvenc_encoder.hpp"
#include "log.hpp"

static GUID PresetGuid(int p) {
    switch (p) {
        case 1: return NV_ENC_PRESET_P1_GUID;
        case 2: return NV_ENC_PRESET_P2_GUID;
        case 3: return NV_ENC_PRESET_P3_GUID;
        case 4: return NV_ENC_PRESET_P4_GUID;
        case 6: return NV_ENC_PRESET_P6_GUID;
        case 7: return NV_ENC_PRESET_P7_GUID;
        case 5:
        default: return NV_ENC_PRESET_P5_GUID;
    }
}

static std::string NvErr(NVENCSTATUS st) {
    return "NVENC status " + std::to_string((int)st);
}

bool NvencEncoder::Init(ID3D11Device* device, const EncoderConfig& cfg, std::string* err) {
    cfg_ = cfg;

    lib_ = LoadLibraryA("nvEncodeAPI64.dll");
    if (!lib_) {
        *err = "nvEncodeAPI64.dll not found (is an NVIDIA driver installed?)";
        return false;
    }
    auto createInstance = reinterpret_cast<NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*)>(
        GetProcAddress(lib_, "NvEncodeAPICreateInstance"));
    if (!createInstance) {
        *err = "NvEncodeAPICreateInstance not exported";
        return false;
    }
    fn_ = {};
    fn_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = createInstance(&fn_);
    if (st != NV_ENC_SUCCESS) {
        *err = NvErr(st);
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op = {};
    op.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    op.device = device;
    op.apiVersion = NVENCAPI_VERSION;
    st = fn_.nvEncOpenEncodeSessionEx(&op, &session_);
    if (st != NV_ENC_SUCCESS) {
        *err = NvErr(st);
        return false;
    }
    open_ = true;

    const GUID codec = cfg.hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    const GUID preset = PresetGuid(cfg.preset);

    NV_ENC_PRESET_CONFIG pc = {};
    pc.version = NV_ENC_PRESET_CONFIG_VER;
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    st = fn_.nvEncGetEncodePresetConfigEx(session_, codec, preset, NV_ENC_TUNING_INFO_HIGH_QUALITY,
                                          &pc);
    if (st != NV_ENC_SUCCESS) {
        *err = NvErr(st);
        return false;
    }

    NV_ENC_CONFIG conf = pc.presetCfg;
    conf.gopLength = cfg.fps * cfg.gopSeconds;
    conf.frameIntervalP = cfg.bframes + 1;
    // Deterministic one-in/one-out ordering: no lookahead-driven reordering.
    conf.rcParams.enableLookahead = 0;

    if (cfg.cbr) {
        conf.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        conf.rcParams.averageBitRate = cfg.bitrateKbps * 1000;
        conf.rcParams.maxBitRate = cfg.bitrateKbps * 1000;
        conf.rcParams.vbvBufferSize = cfg.bitrateKbps * 1000 * 2;
        conf.rcParams.vbvInitialDelay = conf.rcParams.vbvBufferSize;
    } else {
        conf.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        conf.rcParams.constQP.qpIntra = cfg.cqp;
        conf.rcParams.constQP.qpInterP = cfg.cqp;
        conf.rcParams.constQP.qpInterB = cfg.cqp;
    }

    NV_ENC_INITIALIZE_PARAMS ip = {};
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = codec;
    ip.presetGUID = preset;
    ip.encodeWidth = cfg.width;
    ip.encodeHeight = cfg.height;
    ip.darWidth = cfg.width;
    ip.darHeight = cfg.height;
    ip.frameRateNum = cfg.fps;
    ip.frameRateDen = 1;
    ip.enablePTD = 1;
    ip.encodeConfig = &conf;
    ip.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;

    st = fn_.nvEncInitializeEncoder(session_, &ip);
    if (st != NV_ENC_SUCCESS && conf.frameIntervalP > 1) {
        // Turing/Ampere differ on B-frame support depending on codec; retry flat.
        conf.frameIntervalP = 1;
        st = fn_.nvEncInitializeEncoder(session_, &ip);
    }
    if (st != NV_ENC_SUCCESS) {
        *err = NvErr(st);
        return false;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER bb = {};
    bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    st = fn_.nvEncCreateBitstreamBuffer(session_, &bb);
    if (st != NV_ENC_SUCCESS) {
        *err = NvErr(st);
        return false;
    }
    bitstream_ = bb.bitstreamBuffer;
    return true;
}

bool NvencEncoder::RegisterTexture(ID3D11Texture2D* tex, std::string* err) {
    NV_ENC_REGISTER_RESOURCE r = {};
    r.version = NV_ENC_REGISTER_RESOURCE_VER;
    r.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    r.width = cfg_.width;
    r.height = cfg_.height;
    r.pitch = 0;
    r.resourceToRegister = tex;
    r.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    r.bufferUsage = NV_ENC_INPUT_IMAGE;
    NVENCSTATUS st = fn_.nvEncRegisterResource(session_, &r);
    if (st != NV_ENC_SUCCESS) {
        *err = NvErr(st);
        return false;
    }
    registered_ = r.registeredResource;
    return true;
}

bool NvencEncoder::Encode(ID3D11Texture2D* tex, uint64_t frameIdx, bool forceIdr,
                          std::vector<uint8_t>* out, bool* keyframe, int64_t* outPts,
                          std::string* err) {
    (void)tex;  // the texture was registered up-front; NVENC reads it directly on the GPU

    NV_ENC_MAP_INPUT_RESOURCE mp = {};
    mp.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mp.registeredResource = registered_;
    NVENCSTATUS st = fn_.nvEncMapInputResource(session_, &mp);
    if (st != NV_ENC_SUCCESS) {
        *err = NvErr(st);
        return false;
    }

    NV_ENC_PIC_PARAMS pp = {};
    pp.version = NV_ENC_PIC_PARAMS_VER;
    pp.inputWidth = cfg_.width;
    pp.inputHeight = cfg_.height;
    pp.inputPitch = 0;
    pp.outputBitstream = bitstream_;
    pp.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    pp.inputBuffer = mp.mappedResource;
    pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pp.frameIdx = (uint32_t)frameIdx;
    pp.inputTimeStamp = frameIdx;
    pp.encodePicFlags = forceIdr ? NV_ENC_PIC_FLAG_FORCEIDR : 0;
    st = fn_.nvEncEncodePicture(session_, &pp);
    fn_.nvEncUnmapInputResource(session_, mp.mappedResource);
    if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
        // Normal with B-frames/lookahead: input accepted, no packet yet.
        out->clear();
        *keyframe = false;
        *outPts = -1;
        return true;
    }
    if (st != NV_ENC_SUCCESS) {
        *err = NvErr(st);
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lb = {};
    lb.version = NV_ENC_LOCK_BITSTREAM_VER;
    lb.outputBitstream = bitstream_;
    lb.doNotWait = 0;
    st = fn_.nvEncLockBitstream(session_, &lb);
    if (st != NV_ENC_SUCCESS) {
        *err = NvErr(st);
        return false;
    }

    out->assign(reinterpret_cast<uint8_t*>(lb.bitstreamBufferPtr),
                reinterpret_cast<uint8_t*>(lb.bitstreamBufferPtr) + lb.bitstreamSizeInBytes);
    *keyframe = (lb.pictureType == NV_ENC_PIC_TYPE_IDR) || (lb.pictureType == NV_ENC_PIC_TYPE_I);
    *outPts = (int64_t)lb.outputTimeStamp;
    fn_.nvEncUnlockBitstream(session_, bitstream_);
    return true;
}

std::vector<uint8_t> NvencEncoder::SequenceHeader() {
    std::vector<uint8_t> buf(4096);
    uint32_t size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD sp = {};
    sp.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    sp.spsppsBuffer = buf.data();
    sp.inBufferSize = (uint32_t)buf.size();
    sp.outSPSPPSPayloadSize = &size;
    if (fn_.nvEncGetSequenceParams(session_, &sp) != NV_ENC_SUCCESS) return {};
    buf.resize(size);
    return buf;
}

void NvencEncoder::Shutdown() {
    if (open_) {
        if (bitstream_) {
            fn_.nvEncDestroyBitstreamBuffer(session_, bitstream_);
            bitstream_ = nullptr;
        }
        if (registered_) {
            fn_.nvEncUnregisterResource(session_, registered_);
            registered_ = nullptr;
        }
        if (session_) {
            fn_.nvEncDestroyEncoder(session_);
            session_ = nullptr;
        }
        open_ = false;
    }
    if (lib_) {
        FreeLibrary(lib_);
        lib_ = nullptr;
    }
}
