// Nintendo Land Recomp - H.264 kodieren/dekodieren mit Media Foundation (siehe nl_video.h)
#include "nl_video.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX   // sonst macht windows.h aus std::max ein Makro
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wmcodecdsp.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "ole32.lib")

template <typename T> static void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

static uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ===========================================================================
// Farbraum
// ===========================================================================
static uint8_t s_srgb_lut[256];
static bool s_lut_ready = false;
static void make_lut() {
    if (s_lut_ready) return;
    for (int i = 0; i < 256; i++) {
        double c = i / 255.0;
        double e = c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
        s_srgb_lut[i] = (uint8_t)std::clamp((int)std::lround(e * 255.0), 0, 255);
    }
    s_lut_ready = true;
}

void nl_rgba_to_nv12(const uint8_t* rgba, int sw, int sh, int sstride, bool srgb_encode, uint8_t* nv12, int w, int h) {
    make_lut();
    uint8_t* Y = nv12;
    uint8_t* UV = nv12 + (size_t)w * h;
    const bool same = sw == w && sh == h;
    auto px = [&](int x, int y) -> const uint8_t* {
        int sx = same ? x : x * sw / w, sy = same ? y : y * sh / h;
        return rgba + (size_t)sy * sstride + (size_t)sx * 4;
    };
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) {
            int su = 0, sv = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    const uint8_t* p = px(x + dx, y + dy);
                    int r = p[0], g = p[1], b = p[2];
                    if (srgb_encode) { r = s_srgb_lut[r]; g = s_srgb_lut[g]; b = s_srgb_lut[b]; }
                    Y[(size_t)(y + dy) * w + x + dx] = (uint8_t)((47 * r + 157 * g + 16 * b + 128) / 256 + 16);
                    su += (-26 * r - 87 * g + 112 * b);
                    sv += (112 * r - 102 * g - 10 * b);
                }
            uint8_t* uv = UV + (size_t)(y / 2) * w + x;
            uv[0] = (uint8_t)std::clamp((su / 4 + 128) / 256 + 128, 0, 255);
            uv[1] = (uint8_t)std::clamp((sv / 4 + 128) / 256 + 128, 0, 255);
        }
    }
}

void nl_nv12_to_bgra(const uint8_t* Yp, const uint8_t* UVp, int pitch, int w, int h, uint8_t* bgra) {
    for (int y = 0; y < h; y++) {
        const uint8_t* yr = Yp + (size_t)y * pitch;
        const uint8_t* uvr = UVp + (size_t)(y / 2) * pitch;
        uint8_t* o = bgra + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            int c = (yr[x] - 16) * 1192;
            int u = uvr[x & ~1] - 128, v = uvr[(x & ~1) + 1] - 128;
            int r = (c + 1836 * v) >> 10;
            int g = (c - 218 * u - 547 * v) >> 10;
            int b = (c + 2163 * u) >> 10;
            o[0] = (uint8_t)std::clamp(b, 0, 255);
            o[1] = (uint8_t)std::clamp(g, 0, 255);
            o[2] = (uint8_t)std::clamp(r, 0, 255);
            o[3] = 255;
            o += 4;
        }
    }
}

// ===========================================================================
// Encoder
// ===========================================================================
static void codec_set_u4(ICodecAPI* api, const GUID& id, ULONG v) {
    if (!api) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    api->SetValue(&id, &var);
}
static void codec_set_bool(ICodecAPI* api, const GUID& id, bool v) {
    if (!api) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&id, &var);
}

struct EncState {
    IMFTransform* mft = nullptr;
    ICodecAPI* api = nullptr;
    IMFMediaEventGenerator* events = nullptr;   // nur bei asynchronen (Hardware-)Encodern
    bool async = false;
    DWORD in_id = 0, out_id = 0;
    void release() { safe_release(events); safe_release(api); safe_release(mft); }
};

static bool configure_encoder(EncState& st, int w, int h, int fps, int kbps) {
    IMFAttributes* attr = nullptr;
    if (SUCCEEDED(st.mft->GetAttributes(&attr)) && attr) {
        UINT32 async = 0;
        if (SUCCEEDED(attr->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async) {
            attr->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            st.async = true;
        }
        attr->SetUINT32(MF_LOW_LATENCY, TRUE);
        attr->Release();
    }
    DWORD ins = 0, outs = 0;
    if (SUCCEEDED(st.mft->GetStreamCount(&ins, &outs)) && ins == 1 && outs == 1) {
        DWORD a = 0, b = 0;
        if (st.mft->GetStreamIDs(1, &a, 1, &b) == S_OK) { st.in_id = a; st.out_id = b; }
    }
    st.mft->QueryInterface(IID_PPV_ARGS(&st.api));
    if (st.api) {
        codec_set_bool(st.api, CODECAPI_AVLowLatencyMode, true);
        codec_set_u4(st.api, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        codec_set_u4(st.api, CODECAPI_AVEncCommonMeanBitRate, (ULONG)kbps * 1000);
        codec_set_u4(st.api, CODECAPI_AVEncMPVGOPSize, (ULONG)fps);   // jede Sekunde ein Keyframe: Fehler heilen schnell
        codec_set_u4(st.api, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    }

    IMFMediaType* out = nullptr;
    if (FAILED(MFCreateMediaType(&out))) return false;
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kbps * 1000);
    MFSetAttributeSize(out, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(out, MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    HRESULT hr = st.mft->SetOutputType(st.out_id, out, 0);
    out->Release();
    if (FAILED(hr)) return false;

    IMFMediaType* in = nullptr;
    if (FAILED(MFCreateMediaType(&in))) return false;
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(in, MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    in->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    hr = st.mft->SetInputType(st.in_id, in, 0);
    in->Release();
    if (FAILED(hr)) return false;

    if (st.async && FAILED(st.mft->QueryInterface(IID_PPV_ARGS(&st.events)))) return false;
    st.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    st.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

static bool open_encoder(EncState& st, int w, int h, int fps, int kbps, std::string& name) {
    MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in_info, &out_info, &acts, &count))) {
        bool ok = false;
        for (UINT32 i = 0; i < count; i++) {
            if (!ok) {
                st = {};
                if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&st.mft))) && configure_encoder(st, w, h, fps, kbps)) {
                    ok = true;
                    WCHAR* fn = nullptr;
                    UINT32 len = 0;
                    if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fn, &len)) && fn) {
                        name.clear();
                        for (UINT32 k = 0; k < len; k++) name += (char)(fn[k] < 128 ? fn[k] : '?');
                        CoTaskMemFree(fn);
                    } else {
                        name = "Grafikkarte";
                    }
                } else {
                    st.release();
                    acts[i]->ShutdownObject();
                }
            }
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
        if (ok) return true;
    }
    // Software-Encoder von Windows
    st = {};
    if (FAILED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&st.mft)))) return false;
    if (!configure_encoder(st, w, h, fps, kbps)) { st.release(); return false; }
    name = "Windows (CPU)";
    return true;
}

bool NLVideoEncoder::start(int width, int height, int fps, int kbps, PacketFn on_packet) {
    stop();
    m_w = width & ~1;
    m_h = height & ~1;
    m_fps = fps;
    m_kbps = kbps;
    m_on_packet = std::move(on_packet);
    m_stop = false;
    m_want_key = true;
    m_has_frame = false;
    m_started_ok = false;
    m_start_done = false;
    m_thread = std::thread(&NLVideoEncoder::worker, this);
    // auf das Ergebnis der Einrichtung warten (max. 5 s)
    for (int i = 0; i < 500 && !m_start_done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    m_running = m_started_ok;
    if (!m_running) stop();
    return m_running;
}

void NLVideoEncoder::stop() {
    m_stop = true;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
}

std::string NLVideoEncoder::encoder_name() const {
    std::scoped_lock l(m_name_mx);
    return m_name;
}

void NLVideoEncoder::submit_rgba(const uint8_t* rgba, int width, int height, int stride, bool srgb_encode) {
    if (!m_running) return;
    {
        std::scoped_lock l(m_mx);
        m_rgba.resize((size_t)stride * height);
        memcpy(m_rgba.data(), rgba, m_rgba.size());
        m_src_w = width;
        m_src_h = height;
        m_srgb = srgb_encode;
        m_has_frame = true;
    }
    m_cv.notify_one();
}

void NLVideoEncoder::worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    EncState st;
    std::string name;
    const bool ok = open_encoder(st, m_w, m_h, m_fps, m_kbps, name);
    {
        std::scoped_lock l(m_name_mx);
        m_name = ok ? name : "";
    }
    m_started_ok = ok;
    m_start_done = true;
    if (!ok) {
        MFShutdown();
        CoUninitialize();
        return;
    }

    std::vector<uint8_t> rgba, nv12((size_t)m_w * m_h * 3 / 2);
    int need_input = st.async ? 0 : 1;
    const LONGLONG frame_dur = 10000000LL / m_fps;
    const uint64_t t0 = now_us();
    int fed = 0, produced = 0;   // fuer den Wechsel auf den Software-Encoder, falls die Grafikkarte nichts liefert

    auto drain_output = [&]() -> bool {   // true = ein Paket erhalten
        MFT_OUTPUT_STREAM_INFO info{};
        if (FAILED(st.mft->GetOutputStreamInfo(st.out_id, &info))) return false;
        const bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        IMFSample* own = nullptr;
        if (!provides) {
            IMFMediaBuffer* buf = nullptr;
            DWORD size = (std::max<DWORD>)(info.cbSize, (DWORD)(m_w * m_h * 2));
            if (FAILED(MFCreateMemoryBuffer(size, &buf))) return false;
            MFCreateSample(&own);
            own->AddBuffer(buf);
            buf->Release();
        }
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.dwStreamID = st.out_id;
        ob.pSample = own;
        DWORD status = 0;
        HRESULT hr = st.mft->ProcessOutput(0, 1, &ob, &status);
        safe_release(ob.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            IMFMediaType* t = nullptr;
            if (SUCCEEDED(st.mft->GetOutputAvailableType(st.out_id, 0, &t))) { st.mft->SetOutputType(st.out_id, t, 0); t->Release(); }
            safe_release(own);
            return false;
        }
        bool got = false;
        if (SUCCEEDED(hr) && ob.pSample) {
            IMFMediaBuffer* cb = nullptr;
            if (SUCCEEDED(ob.pSample->ConvertToContiguousBuffer(&cb))) {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(cb->Lock(&p, nullptr, &len))) {
                    UINT32 key = 0;
                    ob.pSample->GetUINT32(MFSampleExtension_CleanPoint, &key);
                    LONGLONG ts = 0;
                    ob.pSample->GetSampleTime(&ts);
                    if (m_on_packet && len) m_on_packet(p, len, key != 0, (uint64_t)(ts / 10));
                    produced++;
                    cb->Unlock();
                    got = true;
                }
                cb->Release();
            }
        }
        if (ob.pSample != own) safe_release(ob.pSample);
        safe_release(own);
        return got;
    };

    auto feed = [&]() -> bool {   // true = Bild abgegeben
        {
            std::unique_lock l(m_mx);
            if (!m_has_frame) return false;
            rgba.swap(m_rgba);
            m_has_frame = false;
        }
        nl_rgba_to_nv12(rgba.data(), m_src_w, m_src_h, m_src_w * 4, m_srgb, nv12.data(), m_w, m_h);
        IMFMediaBuffer* buf = nullptr;
        if (FAILED(MFCreateMemoryBuffer((DWORD)nv12.size(), &buf))) return false;
        BYTE* p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        memcpy(p, nv12.data(), nv12.size());
        buf->Unlock();
        buf->SetCurrentLength((DWORD)nv12.size());
        IMFSample* s = nullptr;
        MFCreateSample(&s);
        s->AddBuffer(buf);
        buf->Release();
        s->SetSampleTime((LONGLONG)(now_us() - t0) * 10);
        s->SetSampleDuration(frame_dur);
        if (m_want_key.exchange(false)) codec_set_u4(st.api, CODECAPI_AVEncVideoForceKeyFrame, 1);
        if (int k = m_new_kbps.exchange(0)) {
            m_kbps = k;
            codec_set_u4(st.api, CODECAPI_AVEncCommonMeanBitRate, (ULONG)k * 1000);
        }
        HRESULT hr = st.mft->ProcessInput(st.in_id, s, 0);
        s->Release();
        if (SUCCEEDED(hr)) fed++;
        return SUCCEEDED(hr);
    };

    while (!m_stop) {
        if (st.async && fed >= 90 && produced == 0) {
            // Hardware-Encoder liefert nichts -> Windows-Software-Encoder
            IMFShutdown* sd = nullptr;
            if (SUCCEEDED(st.mft->QueryInterface(IID_PPV_ARGS(&sd)))) { sd->Shutdown(); sd->Release(); }
            st.release();
            st = {};
            if (FAILED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&st.mft))) ||
                !configure_encoder(st, m_w, m_h, m_fps, m_kbps)) {
                st.release();
                break;
            }
            {
                std::scoped_lock l(m_name_mx);
                m_name = "Windows (CPU)";
            }
            need_input = 1;
            fed = 0;
            m_want_key = true;
            continue;
        }
        if (st.async) {
            IMFMediaEvent* ev = nullptr;
            HRESULT hr = st.events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                if (need_input > 0) {
                    std::unique_lock l(m_mx);
                    m_cv.wait_for(l, std::chrono::milliseconds(2), [&] { return m_has_frame || m_stop.load(); });
                    l.unlock();
                    if (feed()) need_input--;
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
                continue;
            }
            if (FAILED(hr) || !ev) break;
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            ev->Release();
            if (type == METransformNeedInput) {
                need_input++;
                if (feed()) need_input--;
            } else if (type == METransformHaveOutput) {
                drain_output();
            }
        } else {
            {
                std::unique_lock l(m_mx);
                m_cv.wait_for(l, std::chrono::milliseconds(20), [&] { return m_has_frame || m_stop.load(); });
            }
            if (m_stop) break;
            if (feed())
                while (drain_output()) {}
        }
    }
    if (st.mft) {
        st.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        st.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFShutdown* sd = nullptr;
        if (SUCCEEDED(st.mft->QueryInterface(IID_PPV_ARGS(&sd)))) { sd->Shutdown(); sd->Release(); }
    }
    st.release();
    MFShutdown();
    CoUninitialize();
}

// ===========================================================================
// Decoder
// ===========================================================================
NLVideoDecoder::~NLVideoDecoder() { reset(); }

void NLVideoDecoder::reset() {
    IMFTransform* mft = (IMFTransform*)m_mft;
    safe_release(mft);
    m_mft = nullptr;
    m_ok = false;
}

bool NLVideoDecoder::init() {
    reset();
    IMFTransform* mft = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft)))) return false;
    m_mft = mft;
    IMFAttributes* attr = nullptr;
    if (SUCCEEDED(mft->GetAttributes(&attr)) && attr) {
        attr->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        attr->Release();
    }
    ICodecAPI* api = nullptr;
    if (SUCCEEDED(mft->QueryInterface(IID_PPV_ARGS(&api)))) {
        codec_set_bool(api, CODECAPI_AVLowLatencyMode, true);
        api->Release();
    }
    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    HRESULT hr = mft->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr) || !configure_output()) { reset(); return false; }
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_ok = true;
    return true;
}

bool NLVideoDecoder::configure_output() {
    IMFTransform* mft = (IMFTransform*)m_mft;
    for (DWORD i = 0;; i++) {
        IMFMediaType* t = nullptr;
        if (FAILED(mft->GetOutputAvailableType(0, i, &t))) return false;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            HRESULT hr = mft->SetOutputType(0, t, 0);
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
            m_alloc_h = (int)h;
            m_w = (int)w;
            m_h = (int)h;
            MFVideoArea area{};
            if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), nullptr))) {
                if (area.Area.cx > 0 && area.Area.cy > 0) { m_w = area.Area.cx; m_h = area.Area.cy; }
            }
            UINT32 stride = 0;
            m_stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (int)stride : (int)w;
            t->Release();
            return SUCCEEDED(hr);
        }
        t->Release();
    }
}

bool NLVideoDecoder::decode(const uint8_t* data, size_t size, uint64_t t_us, std::vector<uint8_t>& bgra, int& width, int& height) {
    if (!m_ok && !init()) return false;
    IMFTransform* mft = (IMFTransform*)m_mft;
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(MFCreateMemoryBuffer((DWORD)size, &buf))) return false;
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    memcpy(p, data, size);
    buf->Unlock();
    buf->SetCurrentLength((DWORD)size);
    IMFSample* s = nullptr;
    MFCreateSample(&s);
    s->AddBuffer(buf);
    buf->Release();
    s->SetSampleTime((LONGLONG)t_us * 10);
    HRESULT hr = mft->ProcessInput(0, s, 0);
    s->Release();
    if (hr == MF_E_NOTACCEPTING) {   // Rueckstau: alte Bilder verwerfen
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        return false;
    }
    if (FAILED(hr)) return false;

    bool got = false;
    for (int guard = 0; guard < 4; guard++) {
        MFT_OUTPUT_STREAM_INFO info{};
        mft->GetOutputStreamInfo(0, &info);
        IMFMediaBuffer* ob_buf = nullptr;
        DWORD need = (std::max<DWORD>)(info.cbSize, (DWORD)((std::max)(m_stride, m_w) * (std::max)(m_alloc_h, m_h) * 3 / 2));
        if (FAILED(MFCreateMemoryBuffer(need, &ob_buf))) break;
        IMFSample* os = nullptr;
        MFCreateSample(&os);
        os->AddBuffer(ob_buf);
        ob_buf->Release();
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.pSample = os;
        DWORD status = 0;
        hr = mft->ProcessOutput(0, 1, &ob, &status);
        safe_release(ob.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            os->Release();
            if (!configure_output()) { reset(); return false; }
            continue;
        }
        if (FAILED(hr)) { os->Release(); break; }   // MF_E_TRANSFORM_NEED_MORE_INPUT
        IMFMediaBuffer* cb = nullptr;
        if (SUCCEEDED(os->ConvertToContiguousBuffer(&cb))) {
            BYTE* src = nullptr;
            LONG pitch = 0;
            IMF2DBuffer* b2 = nullptr;
            bool locked2d = false;
            if (SUCCEEDED(cb->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&src, &pitch))) locked2d = true;
            else {
                safe_release(b2);
                cb->Lock(&src, nullptr, nullptr);
                pitch = m_stride > 0 ? m_stride : m_w;
            }
            if (src && pitch > 0) {
                bgra.resize((size_t)m_w * m_h * 4);
                nl_nv12_to_bgra(src, src + (size_t)pitch * m_alloc_h, (int)pitch, m_w, m_h, bgra.data());
                width = m_w;
                height = m_h;
                got = true;
            }
            if (locked2d) { b2->Unlock2D(); b2->Release(); }
            else cb->Unlock();
            cb->Release();
        }
        os->Release();
    }
    return got;
}
