// Hardware H.264 / AV1 video encoder via Windows Media Foundation (IMFTransform).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>

#include <cstring>
#include <vector>

extern "C" {
#include "host.h"
#include "stream_hwenc.h"
}

static const GUID kGuidH264 = {0x34363248, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kGuidAV1  = {0x31305641, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kGuidNV12 = {0x3231564e, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kGuidMeanBitrate = {0xb7101f4e, 0x3b8a, 0x4b42, {0x88, 0x62, 0x85, 0x5c, 0x2a, 0x45, 0x20, 0x15}};
static const GUID kGuidICodecAPI   = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};

static bool g_mf_inited = false;
static int g_probe_mask = -1;

static IMFTransform *g_mft = nullptr;
static IMFMediaEventGenerator *g_evgen = nullptr;
static ICodecAPI *g_codec_api = nullptr;
static bool g_is_async = false;
static bool g_out_provides_samples = false;
static int g_w = 0, g_h = 0, g_fps = 60;
static uint64_t g_frame_idx = 0;
static std::vector<uint8_t> g_out_pkt;

static void ensure_mf(void) {
    if (!g_mf_inited) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) g_mf_inited = true;
    }
}

static bool has_hw_mft(const GUID &subtype) {
    ensure_mf();
    if (!g_mf_inited) return false;
    MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Video, subtype};
    IMFActivate **acts = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           nullptr, &out_info, &acts, &count);
    bool found = SUCCEEDED(hr) && count > 0;
    if (acts) {
        for (UINT32 i = 0; i < count; i++) {
            if (acts[i]) acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }
    return found;
}

int stream_hwenc_probe(void) {
    if (g_probe_mask >= 0) return g_probe_mask;
    int mask = 0;
    if (has_hw_mft(kGuidH264)) mask |= (1 << STREAM_CODEC_H264);
    if (has_hw_mft(kGuidAV1)) mask |= (1 << STREAM_CODEC_AV1);
    g_probe_mask = mask;
    rt_log("stream: GPU hardware encoders detected: H.264=%s, AV1=%s",
           (mask & (1 << STREAM_CODEC_H264)) ? "yes" : "no",
           (mask & (1 << STREAM_CODEC_AV1)) ? "yes" : "no");
    return mask;
}

void stream_hwenc_close(void) {
    if (g_mft) {
        g_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        g_mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    if (g_codec_api) { g_codec_api->Release(); g_codec_api = nullptr; }
    if (g_evgen) { g_evgen->Release(); g_evgen = nullptr; }
    if (g_mft) { g_mft->Release(); g_mft = nullptr; }
    g_is_async = false;
    g_out_provides_samples = false;
    g_w = g_h = 0;
    g_frame_idx = 0;
}

static void set_codec_u32(const GUID &key, UINT32 val) {
    if (!g_codec_api) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = val;
    g_codec_api->SetValue(&key, &v);
}

static void set_codec_bool(const GUID &key, bool val) {
    if (!g_codec_api) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
    g_codec_api->SetValue(&key, &v);
}

int stream_hwenc_open(int codec, int w, int h, int fps, int kbps, int kf_dist) {
    stream_hwenc_close();
    ensure_mf();
    if (!g_mf_inited) return 0;

    const GUID &subtype = (codec == STREAM_CODEC_AV1) ? kGuidAV1
                        : (codec == STREAM_CODEC_H264) ? kGuidH264
                        : GUID_NULL;
    if (subtype == GUID_NULL) return 0;

    MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Video, subtype};
    IMFActivate **acts = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           nullptr, &out_info, &acts, &count);
    if (FAILED(hr) || !count || !acts) {
        if (acts) CoTaskMemFree(acts);
        return 0;
    }

    hr = acts[0]->ActivateObject(IID_IMFTransform, (void **)&g_mft);
    for (UINT32 i = 0; i < count; i++) {
        if (acts[i]) acts[i]->Release();
    }
    CoTaskMemFree(acts);
    if (FAILED(hr) || !g_mft) {
        stream_hwenc_close();
        return 0;
    }

    IMFAttributes *attrs = nullptr;
    if (SUCCEEDED(g_mft->GetAttributes(&attrs)) && attrs) {
        UINT32 is_async = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async) {
            g_is_async = true;
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            g_mft->QueryInterface(IID_IMFMediaEventGenerator, (void **)&g_evgen);
        }
        attrs->Release();
    }

    g_mft->QueryInterface(kGuidICodecAPI, (void **)&g_codec_api);
    set_codec_bool(CODECAPI_AVLowLatencyMode, true);
    set_codec_u32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    set_codec_u32(kGuidMeanBitrate, (UINT32)kbps * 1000u);
    set_codec_u32(CODECAPI_AVEncMPVGOPSize, (UINT32)kf_dist);
    set_codec_u32(CODECAPI_AVEncCommonQualityVsSpeed, 70);

    IMFMediaType *out_mt = nullptr;
    if (FAILED(MFCreateMediaType(&out_mt))) {
        stream_hwenc_close();
        return 0;
    }
    out_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_mt->SetGUID(MF_MT_SUBTYPE, subtype);
    out_mt->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kbps * 1000u);
    out_mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(out_mt, MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
    MFSetAttributeRatio(out_mt, MF_MT_FRAME_RATE, (UINT32)fps, 1);
    MFSetAttributeRatio(out_mt, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (codec == STREAM_CODEC_H264)
        out_mt->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);

    hr = g_mft->SetOutputType(0, out_mt, 0);
    out_mt->Release();
    if (FAILED(hr)) {
        rt_log("stream: hwenc SetOutputType failed (%#lx) for codec=%d", (unsigned long)hr, codec);
        stream_hwenc_close();
        return 0;
    }

    IMFMediaType *in_mt = nullptr;
    if (FAILED(MFCreateMediaType(&in_mt))) {
        stream_hwenc_close();
        return 0;
    }
    in_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_mt->SetGUID(MF_MT_SUBTYPE, kGuidNV12);
    in_mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in_mt->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)w);
    MFSetAttributeSize(in_mt, MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
    MFSetAttributeRatio(in_mt, MF_MT_FRAME_RATE, (UINT32)fps, 1);
    MFSetAttributeRatio(in_mt, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = g_mft->SetInputType(0, in_mt, 0);
    in_mt->Release();
    if (FAILED(hr)) {
        rt_log("stream: hwenc SetInputType(NV12) failed (%#lx) for codec=%d", (unsigned long)hr, codec);
        stream_hwenc_close();
        return 0;
    }

    MFT_OUTPUT_STREAM_INFO osi{};
    if (SUCCEEDED(g_mft->GetOutputStreamInfo(0, &osi))) {
        g_out_provides_samples = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    }

    g_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    g_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    g_w = w;
    g_h = h;
    g_fps = fps;
    g_frame_idx = 0;
    rt_log("stream: GPU hardware encoder opened (%s, %dx%d @ %d kbps, async=%d)",
           codec == STREAM_CODEC_AV1 ? "AV1" : "H.264", w, h, kbps, g_is_async ? 1 : 0);
    return 1;
}

static int drain_output(const uint8_t **out_buf, size_t *out_len, int *out_kf) {
    MFT_OUTPUT_DATA_BUFFER odb{};
    IMFSample *out_sample = nullptr;
    IMFMediaBuffer *out_mb = nullptr;
    if (!g_out_provides_samples) {
        if (FAILED(MFCreateSample(&out_sample))) return 0;
        if (FAILED(MFCreateMemoryBuffer((DWORD)(g_w * g_h * 2), &out_mb))) {
            out_sample->Release();
            return 0;
        }
        out_sample->AddBuffer(out_mb);
        odb.pSample = out_sample;
    }
    DWORD status = 0;
    HRESULT hr = g_mft->ProcessOutput(0, 1, &odb, &status);
    if (odb.pEvents) odb.pEvents->Release();

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        IMFMediaType *new_mt = nullptr;
        if (SUCCEEDED(g_mft->GetOutputAvailableType(0, 0, &new_mt)) && new_mt) {
            g_mft->SetOutputType(0, new_mt, 0);
            new_mt->Release();
        }
        if (out_mb) out_mb->Release();
        if (out_sample) out_sample->Release();
        return 0;
    }

    int got = 0;
    if (SUCCEEDED(hr) && odb.pSample) {
        UINT32 cp = 0;
        odb.pSample->GetUINT32(MFSampleExtension_CleanPoint, &cp);
        *out_kf = cp ? 1 : 0;
        IMFMediaBuffer *buf = nullptr;
        if (SUCCEEDED(odb.pSample->ConvertToContiguousBuffer(&buf)) && buf) {
            BYTE *ptr = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&ptr, nullptr, &len)) && ptr && len > 0) {
                g_out_pkt.assign(ptr, ptr + len);
                *out_buf = g_out_pkt.data();
                *out_len = g_out_pkt.size();
                got = 1;
                buf->Unlock();
            }
            buf->Release();
        }
        if (g_out_provides_samples) odb.pSample->Release();
    }
    if (out_mb) out_mb->Release();
    if (out_sample) out_sample->Release();
    return got;
}

int stream_hwenc_encode(const uint8_t *nv12, int w, int h, int force_kf,
                        const uint8_t **out_buf, size_t *out_len, int *out_kf) {
    if (!g_mft || !nv12 || w != g_w || h != g_h) return 0;

    if (force_kf) set_codec_bool(CODECAPI_AVEncVideoForceKeyFrame, true);

    if (g_is_async && g_evgen) {
        // Pump any pending NeedInput / HaveOutput events quickly
        for (int tries = 0; tries < 8; tries++) {
            IMFMediaEvent *ev = nullptr;
            if (FAILED(g_evgen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev)) || !ev) break;
            MediaEventType met = MEUnknown;
            ev->GetType(&met);
            ev->Release();
            if (met == METransformHaveOutput) {
                if (drain_output(out_buf, out_len, out_kf)) {
                    // Drain this frame first, then feed new input on next call or below
                }
            }
        }
    }

    const size_t nv12_sz = (size_t)w * (size_t)h * 3 / 2;
    IMFSample *in_sample = nullptr;
    IMFMediaBuffer *in_mb = nullptr;
    if (FAILED(MFCreateSample(&in_sample)) || FAILED(MFCreateMemoryBuffer((DWORD)nv12_sz, &in_mb))) {
        if (in_mb) in_mb->Release();
        if (in_sample) in_sample->Release();
        return 0;
    }
    BYTE *dst = nullptr;
    if (SUCCEEDED(in_mb->Lock(&dst, nullptr, nullptr)) && dst) {
        std::memcpy(dst, nv12, nv12_sz);
        in_mb->Unlock();
        in_mb->SetCurrentLength((DWORD)nv12_sz);
    }
    in_sample->AddBuffer(in_mb);
    LONGLONG dur = 10000000LL / (g_fps > 0 ? g_fps : 60);
    in_sample->SetSampleTime((LONGLONG)g_frame_idx * dur);
    in_sample->SetSampleDuration(dur);
    g_frame_idx++;

    HRESULT hr = g_mft->ProcessInput(0, in_sample, 0);
    in_mb->Release();
    in_sample->Release();
    if (FAILED(hr)) return 0;

    if (drain_output(out_buf, out_len, out_kf)) return 1;
    if (g_is_async && g_evgen) {
        for (int tries = 0; tries < 4; tries++) {
            IMFMediaEvent *ev = nullptr;
            if (FAILED(g_evgen->GetEvent(0, &ev)) || !ev) break;
            MediaEventType met = MEUnknown;
            ev->GetType(&met);
            ev->Release();
            if (met == METransformHaveOutput && drain_output(out_buf, out_len, out_kf)) return 1;
            if (met == METransformNeedInput) break;
        }
    }
    return 0;
}
