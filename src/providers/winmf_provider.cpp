#include "winmf_provider.h"
#include <mferror.h>
#include <cassert>
#include <algorithm>
#include <sstream>
#include <comdef.h>  // for _com_error
#include <windows.h> // for OutputDebugStringA
#include "../core/frame_converter.h"

using Microsoft::WRL::ComPtr;

static std::string hr_msg(HRESULT hr)
{
    _com_error ce(hr);
    std::ostringstream oss;
    oss << "hr=0x" << std::hex << std::uppercase << (unsigned long)hr
        << " (" << (ce.ErrorMessage() ? ce.ErrorMessage() : "unknown") << ")";
    return oss.str();
}

#define DBG(stage, hr)                                                          \
    do                                                                          \
    {                                                                           \
        std::string __m = std::string("[WinMF] ") + stage + " : " + hr_msg(hr); \
        OutputDebugStringA((__m + "\n").c_str());                               \
    } while (0)

static void ensure_mf()
{
    static std::once_flag f;
    std::call_once(f, []
                   {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // 如果已經被其他人用不同 apartment 初始化（常見 0x80010106），直接忽略即可
        if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
            DBG("CoInitializeEx", hr);
        }

        hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            DBG("MFStartup", hr);
            // 這通常是 Windows N/Server 未裝 Media Feature Pack
            OutputDebugStringA("[WinMF] Media Foundation platform not initialized. Check 'Media Features' / Media Feature Pack.\n");
        } });
}

static std::string utf8_from_wide(const wchar_t *ws)
{
    if (!ws)
        return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::vector<char> buf(n);
    ::WideCharToMultiByte(CP_UTF8, 0, ws, -1, buf.data(), n, nullptr, nullptr);
    return std::string(buf.data());
}

WinMFProvider::WinMFProvider(bool preferGpu)
    : prefer_gpu_(preferGpu)
{
}
WinMFProvider::~WinMFProvider()
{
    stop();
    close();
}

void WinMFProvider::emit_error(gcap_status_t c, const char *msg)
{
    if (ecb_)
        ecb_(c, msg, user_);
}

bool WinMFProvider::enumerate(std::vector<gcap_device_info_t> &list)
{
    ensure_mf();
    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(&attr, 1)))
        return false;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate **pp = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attr.Get(), &pp, &count)))
        return false;

    list.clear();
    for (UINT32 i = 0; i < count; ++i)
    {
        WCHAR *wname = nullptr;
        UINT32 cch = 0;
        if (SUCCEEDED(pp[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &wname, &cch)))
        {
            gcap_device_info_t di{};
            di.index = (int)i;
            std::string s = utf8_from_wide(wname);
            strncpy(di.name, s.c_str(), sizeof(di.name) - 1);
            di.name[sizeof(di.name) - 1] = '\0';
            di.caps = 0;
            list.push_back(di);
            CoTaskMemFree(wname);
        }
        pp[i]->Release();
    }
    CoTaskMemFree(pp);
    return true;
}

bool WinMFProvider::create_reader_cpu_only(int devIndex)
{
    // enumerate & activate
    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(&attr, 1)))
        return false;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate **pp = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attr.Get(), &pp, &count);
    if (FAILED(hr) || (UINT32)devIndex >= count)
    {
        if (pp)
        {
            for (UINT32 i = 0; i < count; ++i)
                pp[i]->Release();
            CoTaskMemFree(pp);
        }
        return false;
    }
    hr = pp[devIndex]->ActivateObject(__uuidof(IMFMediaSource),
                                      (void **)source_.ReleaseAndGetAddressOf());

    // 讀 FriendlyName 存到 dev_name_
    WCHAR *wname = nullptr;
    UINT32 cch = 0;
    if (SUCCEEDED(pp[devIndex]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &wname, &cch)))
    {
        dev_name_ = utf8_from_wide(wname);
        CoTaskMemFree(wname);
    }

    for (UINT32 i = 0; i < count; ++i)
        pp[i]->Release();
    CoTaskMemFree(pp);
    if (FAILED(hr))
    {
        DBG("ActivateObject(IMFMediaSource)", hr);
        return false;
    }

    // 關鍵：啟用 Video Processing（讓 MFT 幫我們解 MJPG/轉色彩）
    ComPtr<IMFAttributes> rdAttr;
    hr = MFCreateAttributes(&rdAttr, 1);
    if (FAILED(hr))
    {
        DBG("MFCreateAttributes(reader)", hr);
        return false;
    }
    rdAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    hr = MFCreateSourceReaderFromMediaSource(source_.Get(), rdAttr.Get(), &reader_);
    if (FAILED(hr))
    {
        DBG("CreateReader CPU+VP", hr);
        return false;
    }

    return true;
}

bool WinMFProvider::open(int index)
{
    ensure_mf();

    if (prefer_gpu_)
    {
        // 先試 GPU + DXGI 管線
        if (create_d3d() && create_reader_with_dxgi(index) && use_dxgi_)
        {
            GUID sub = GUID_NULL;
            UINT w = 0, h = 0, fn = 0, fd = 1;
            if (pick_best_native(sub, w, h, fn, fd) && ensure_rt_and_pipeline((int)w, (int)h))
            {
                cur_subtype_ = sub;
                cur_w_ = (int)w;
                cur_h_ = (int)h;
                cpu_path_ = false;
                OutputDebugStringA("[WinMF] open(): using DXGI/GPU pipeline\n");
                return true;
            }
            OutputDebugStringA("[WinMF] open(): DXGI path failed after pick_best_native/ensure_rt, fallback to CPU\n");
        }
    }

    // ---- CPU fallback ----
    reader_.Reset();
    source_.Reset();
    use_dxgi_ = false;
    cpu_path_ = true;

    // 只建立 CPU + Video Processing 的 reader
    if (!create_reader_cpu_only(index))
    {
        emit_error(GCAP_EIO, "Create reader (CPU) failed");
        OutputDebugStringA("[WinMF] open(): Create reader (CPU) failed\n");
        return false;
    }

    // 先嘗試把輸出設成 NV12，不行再 ARGB32
    {
        ComPtr<IMFMediaType> mt;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        HRESULT hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt.Get());
        if (FAILED(hr))
        {
            DBG("SetCurrentMediaType(NV12)", hr);
            ComPtr<IMFMediaType> mt2;
            MFCreateMediaType(&mt2);
            mt2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            mt2->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
            hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt2.Get());
            if (FAILED(hr))
            {
                DBG("SetCurrentMediaType(ARGB32)", hr);
                // 兩個都失敗就不硬設，直接用裝置預設型態
            }
        }
    }

    // 讀回實際型態，更新 cur_* 給 CPU 路使用
    ComPtr<IMFMediaType> cur;
    if (FAILED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)))
    {
        emit_error(GCAP_EIO, "GetCurrentMediaType failed");
        return false;
    }

    UINT32 w = 0, h = 0, fn = 0, fd = 1;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &fn, &fd);
    cur_w_ = (int)w;
    cur_h_ = (int)h;
    cur_stride_ = 0;
    cur->GetGUID(MF_MT_SUBTYPE, &cur_subtype_);
    if (cur_subtype_ == MFVideoFormat_ARGB32)
        cur_stride_ = cur_w_ * 4;

    // 只開第一個視訊串流
    reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    OutputDebugStringA("[WinMF] open(): using CPU pipeline\n");
    return true;
}

bool WinMFProvider::setProfile(const gcap_profile_t &p)
{
    profile_ = p;
    return true;
}
bool WinMFProvider::setBuffers(int, size_t) { return true; }

bool WinMFProvider::start()
{
    if (running_)
        return true;
    running_ = true;
    th_ = std::thread(&WinMFProvider::loop, this);
    return true;
}

void WinMFProvider::stop()
{
    if (!running_)
        return;
    running_ = false;
    if (th_.joinable())
        th_.join();
}

void WinMFProvider::close()
{
    reader_.Reset();
    source_.Reset();
    d2d_bitmap_rt_.Reset();
    rtv_rgba_.Reset();
    rt_rgba_.Reset();
    rt_stage_.Reset();
    d2d_ctx_.Reset();
    d2d_device_.Reset();
    d2d_factory_.Reset();
    dwrite_.Reset();
    samp_.Reset();
    vb_.Reset();
    il_.Reset();
    vs_.Reset();
    ps_nv12_.Reset();
    ps_p010_.Reset();
    cs_nv12_.Reset();
    cs_params_.Reset();
    rt_uav_.Reset();

    if (dxgi_mgr_)
        dxgi_mgr_.Reset();
    ctx1_.Reset();
    d3d1_.Reset();
    ctx_.Reset();
    d3d_.Reset();
}

void WinMFProvider::setCallbacks(gcap_on_video_cb vcb, gcap_on_error_cb ecb, void *user)
{
    vcb_ = vcb;
    ecb_ = ecb;
    user_ = user;
}

// -------------------- D3D / MF init --------------------

bool WinMFProvider::create_d3d()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    auto try_create = [&](UINT fl)
    {
        return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, fl,
                                 fls, _countof(fls), D3D11_SDK_VERSION,
                                 &d3d_, &got, &ctx_);
    };

    HRESULT hr = try_create(flags);
#ifdef _DEBUG
    if (FAILED(hr))
    {
        // 自動移除 DEBUG 再試一次
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = try_create(flags);
    }
#endif
    if (FAILED(hr))
    {
        DBG("D3D11CreateDevice", hr);
        return false;
    }

    d3d_.As(&d3d1_);
    ctx_.As(&ctx1_);

    if (FAILED(MFCreateDXGIDeviceManager(&dxgi_token_, &dxgi_mgr_)))
    {
        DBG("MFCreateDXGIDeviceManager", E_FAIL);
        return false;
    }

    ComPtr<IDXGIDevice> dx;
    d3d_.As(&dx);
    if (FAILED(dxgi_mgr_->ResetDevice(dx.Get(), dxgi_token_)))
    {
        DBG("DXGI ResetDevice", E_FAIL);
        return false;
    }

    // D2D / DWrite
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.ReleaseAndGetAddressOf())))
        return false;
    ComPtr<IDXGIDevice> dxgiDev;
    d3d_.As(&dxgiDev);
    if (FAILED(d2d_factory_->CreateDevice(dxgiDev.Get(), &d2d_device_)))
        return false;
    if (FAILED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_ctx_)))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &dwrite_)))
        return false;
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &d2d_white_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.55f), &d2d_black_);
    return true;
}

bool WinMFProvider::create_reader_with_dxgi(int devIndex)
{
    use_dxgi_ = false; // 預設關掉 DXGI
    cpu_path_ = true;

    auto activate_source = [&](ComPtr<IMFMediaSource> &out) -> HRESULT
    {
        out.Reset();
        ComPtr<IMFAttributes> attr;
        HRESULT hr = MFCreateAttributes(&attr, 1);
        if (FAILED(hr))
            return hr;
        attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

        IMFActivate **pp = nullptr;
        UINT32 count = 0;
        hr = MFEnumDeviceSources(attr.Get(), &pp, &count);
        if (FAILED(hr) || (UINT32)devIndex >= count)
        {
            if (pp)
            {
                for (UINT32 i = 0; i < count; ++i)
                    pp[i]->Release();
                CoTaskMemFree(pp);
            }
            return FAILED(hr) ? hr : E_INVALIDARG;
        }

        hr = pp[devIndex]->ActivateObject(__uuidof(IMFMediaSource),
                                          reinterpret_cast<void **>(out.GetAddressOf()));

        for (UINT32 i = 0; i < count; ++i)
            pp[i]->Release();
        CoTaskMemFree(pp);
        return hr;
    };

    HRESULT hr = S_OK;

    DBG("DXGI: Try DXGI+VP - begin", S_OK);
    hr = activate_source(source_);
    if (FAILED(hr))
    {
        DBG("DXGI: activate_source failed", hr);
        return false;
    }

    ComPtr<IMFAttributes> rdAttr;
    hr = MFCreateAttributes(&rdAttr, 3);
    if (FAILED(hr))
    {
        DBG("DXGI: MFCreateAttributes(reader) failed", hr);
        return false;
    }

    rdAttr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgi_mgr_.Get());
    rdAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    rdAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    if (!source_)
    {
        DBG("DXGI: source_ is null before MFCreateSourceReaderFromMediaSource", 0);
        return false;
    }
    if (!dxgi_mgr_)
    {
        DBG("DXGI: dxgi_mgr_ is null before MFCreateSourceReaderFromMediaSource", 0);
        return false;
    }

    hr = MFCreateSourceReaderFromMediaSource(source_.Get(), rdAttr.Get(), reader_.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        DBG("DXGI: MFCreateSourceReaderFromMediaSource (with attr) failed", hr);

        // 如果是你現在遇到的 E_INVALIDARG，就改用「重新 activate 一顆新的 source 再建乾淨 reader」的方式重試
        if (hr == E_INVALIDARG)
        {
            DBG("DXGI: retry MFCreateSourceReaderFromMediaSource with nullptr attr", hr);

            // 先把舊的 reader / source 都清掉，因為舊的 source 很可能已經被 Shutdown
            reader_.Reset();
            source_.Reset();

            // ★ 重新 activate 一顆新的 MediaSource（用同一個 devIndex）
            HRESULT hr2 = activate_source(source_);
            if (FAILED(hr2) || !source_)
            {
                DBG("DXGI: activate_source (retry) failed", hr2);
                source_.Reset();
                return false; // DXGI 這條就放棄，最後 open() 會走 CPU fallback
            }

            // 用「不帶任何 attributes」建一個最乾淨的 reader
            hr = MFCreateSourceReaderFromMediaSource(
                source_.Get(),
                nullptr,
                reader_.ReleaseAndGetAddressOf());
            if (FAILED(hr))
            {
                DBG("DXGI: MFCreateSourceReaderFromMediaSource (no attr) also failed", hr);
                reader_.Reset();
                source_.Reset();
                return false;
            }

            DBG("DXGI: CreateReader(no attr) succeeded, continue DXGI path", hr);
        }
        else
        {
            // 不是 E_INVALIDARG，就當成不支援 DXGI，讓 open() fallback 到 CPU
            reader_.Reset();
            source_.Reset();
            return false;
        }
    }

    // 能走到這裡代表 reader_ 已經成功建立（帶 attr 或無 attr）
    use_dxgi_ = true;
    cpu_path_ = false;
    DBG("DXGI: DXGI+VP SUCCESS", hr);
    return true;
}

bool WinMFProvider::pick_best_native(GUID &sub, UINT32 &w, UINT32 &h, UINT32 &fn, UINT32 &fd)
{
    struct Cand
    {
        GUID sub;
        UINT32 w, h, fn, fd;
        long long score;
        ComPtr<IMFMediaType> mt;
    };
    std::vector<Cand> list;

    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> t;
        HRESULT hr = reader_->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t);
        if (FAILED(hr) && hr != MF_E_NO_MORE_TYPES)
        {
            DBG("GetNativeMediaType", hr);
        }
        if (hr == MF_E_NO_MORE_TYPES)
            break;
        if (FAILED(hr) || !t)
            continue;
        GUID major = GUID_NULL, s = GUID_NULL;
        if (FAILED(t->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video)
            continue;
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &s)))
            continue;

        // 收 P010/NV12/YUY2/MJPG
        if (!(s == MFVideoFormat_P010 || s == MFVideoFormat_NV12 ||
              s == MFVideoFormat_YUY2 || s == MFVideoFormat_MJPG))
            continue;

        UINT32 cw = 0, ch = 0, fnum = 0, fden = 1;
        if (FAILED(MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &cw, &ch)))
            continue;
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &fnum, &fden);
        double fps = fden ? (double)fnum / fden : 0.0;

        int pref = (s == MFVideoFormat_P010) ? 3 : (s == MFVideoFormat_NV12) ? 2
                                               : (s == MFVideoFormat_YUY2)   ? 1
                                                                             : 0;
        long long score = (long long)cw * ch * 100000 + (long long)(fps * 1000) * 100 + pref;
        list.push_back({s, cw, ch, fnum, fden, score, t});
    }
    if (list.empty())
        return false;

    std::sort(list.begin(), list.end(), [](auto &a, auto &b)
              { return a.score > b.score; });
    const auto &best = list[0];

    // 1) 原生就是 P010/NV12 → 直接套用
    if (best.sub == MFVideoFormat_P010 || best.sub == MFVideoFormat_NV12)
    {
        HRESULT hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, best.mt.Get());
        if (FAILED(hr))
        {
            DBG("SetCurrentMediaType(NV12)", hr);
            return false;
        }
        sub = best.sub;
        w = best.w;
        h = best.h;
        fn = best.fn;
        fd = best.fd;
        cpu_path_ = !use_dxgi_; // 沒有 DXGI 紋理仍要 CPU
        return true;
    }

    // 2) 否則（YUY2/MJPG）→ 請 reader 直接輸出 ARGB32（CPU 路徑）
    ComPtr<IMFMediaType> req;
    if (FAILED(MFCreateMediaType(&req)))
        return false;
    req->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    req->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    MFSetAttributeSize(req.Get(), MF_MT_FRAME_SIZE, best.w, best.h);
    MFSetAttributeRatio(req.Get(), MF_MT_FRAME_RATE, best.fn, best.fd);

    HRESULT hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, req.Get());
    if (FAILED(hr))
    {
        DBG("SetCurrentMediaType(ARGB32)", hr);
        return false;
    }

    // 讀回實際（保險）
    ComPtr<IMFMediaType> cur;
    if (FAILED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)))
    {
        DBG("GetCurrentMediaType", E_FAIL);
        return false;
    }

    UINT32 rw = 0, rh = 0, rfn = 0, rfd = 1;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &rw, &rh);
    MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &rfn, &rfd);

    sub = MFVideoFormat_ARGB32;
    w = rw;
    h = rh;
    fn = rfn;
    fd = rfd;
    cpu_path_ = true; // 明確走 CPU
    return true;
}

// -------------------- Pipeline / Rendering --------------------

static const char *g_vs_src = R"(
struct VSIn  { float2 pos:POSITION; float2 uv:TEXCOORD0; };
struct VSOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
VSOut main(VSIn i){
  VSOut o; o.pos=float4(i.pos,0,1); o.uv=i.uv; return o;
}
)";

static const char *g_ps_nv12 = R"(
Texture2D texY   : register(t0);
Texture2D texUV  : register(t1);
SamplerState samL: register(s0);

float3 yuv_to_rgb709(float y, float u, float v)
{
    // y,u,v already normalized 0..1, assume limited->full & BT.709
    y = y * 255.0;
    u = (u - 0.5) * 255.0;
    v = (v - 0.5) * 255.0;
    float c = y - 16.0;
    float d = u;
    float e = v;
    float r = 1.164383 * c + 1.792741 * e;
    float g = 1.164383 * c - 0.213249 * d - 0.532909 * e;
    float b = 1.164383 * c + 2.112402 * d;
    return saturate(float3(r,g,b)/255.0);
}

float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target
{
    float y  = texY .Sample(samL, uv).r;
    float2 uv2= texUV.Sample(samL, uv).rg;
    float3 rgb = yuv_to_rgb709(y, uv2.x, uv2.y);
    return float4(rgb, 1.0);
}
)";

static const char *g_ps_p010 = R"(
Texture2D<uint>  texY16   : register(t0);
Texture2D<uint2> texUV16  : register(t1);
SamplerState samL: register(s0);

float3 yuv_to_rgb709(float y, float u, float v)
{
    y = y * 255.0;
    u = (u - 0.5) * 255.0;
    v = (v - 0.5) * 255.0;
    float c = y - 16.0;
    float d = u;
    float e = v;
    float r = 1.164383 * c + 1.792741 * e;
    float g = 1.164383 * c - 0.213249 * d - 0.532909 * e;
    float b = 1.164383 * c + 2.112402 * d;
    return saturate(float3(r,g,b)/255.0);
}

float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target
{
    uint yy = texY16.Load(int3(pos.xy,0)).r;
    uint2 uvv= texUV16.Load(int3(pos.xy,0)).rg;
    float y = (float)((yy >> 6) & 1023) / 1023.0;
    float u = (float)((uvv.x >> 6) & 1023) / 1023.0;
    float v = (float)((uvv.y >> 6) & 1023) / 1023.0;
    float3 rgb = yuv_to_rgb709(y, u, v);
    return float4(rgb, 1.0);
}
)";

// NV12 → RGBA 的 Compute Shader 版本
static const char *g_cs_nv12 = R"(
Texture2D<float>  texY    : register(t0);  // Y 平面（R8_UNORM → 0..1）
Texture2D<float2> texUV   : register(t1);  // UV 平面（R8G8_UNORM → 0..1）
RWTexture2D<float4> texOut : register(u0); // 輸出 RGBA8

cbuffer Params : register(b0)
{
    uint width;
    uint height;
};

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint x = tid.x;
    uint y = tid.y;

    if (x >= width || y >= height)
        return;

    // 讀取 Y / UV（NV12：2x2 共用一組 UV）
    float  yNorm = texY .Load(int3(x, y, 0));         // 0..1
    float2 uvNorm= texUV.Load(int3(x / 2, y / 2, 0)); // 0..1

    float Y = yNorm * 255.0;
    float U = (uvNorm.x - 0.5) * 255.0;
    float V = (uvNorm.y - 0.5) * 255.0;

    float c = Y - 16.0;
    float d = U;
    float e = V;

    float r = 1.164383 * c + 1.792741 * e;
    float g = 1.164383 * c - 0.213249 * d - 0.532909 * e;
    float b = 1.164383 * c + 2.112402 * d;

    float3 rgb = float3(r, g, b) / 255.0;
    rgb = saturate(rgb);

    texOut[uint2(x, y)] = float4(rgb, 1.0);
}
)";

bool WinMFProvider::create_shaders_and_states()
{
    // Compile shaders
    ComPtr<ID3DBlob> vsb, psb1, psb2, err;
    if (FAILED(D3DCompile(g_vs_src, strlen(g_vs_src), nullptr, nullptr, nullptr,
                          "main", "vs_5_0", 0, 0, &vsb, &err)))
        return false;
    if (FAILED(d3d_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)))
        return false;

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(d3d_->CreateInputLayout(ied, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il_)))
        return false;

    if (FAILED(D3DCompile(g_ps_nv12, strlen(g_ps_nv12), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psb1, &err)))
        return false;
    if (FAILED(d3d_->CreatePixelShader(psb1->GetBufferPointer(), psb1->GetBufferSize(), nullptr, &ps_nv12_)))
        return false;

    if (FAILED(D3DCompile(g_ps_p010, strlen(g_ps_p010), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psb2, &err)))
        return false;
    if (FAILED(d3d_->CreatePixelShader(psb2->GetBufferPointer(), psb2->GetBufferSize(), nullptr, &ps_p010_)))
        return false;

    // Fullscreen quad (two triangles)
    struct V
    {
        float x, y, u, v;
    };
    V quad[6] = {
        {-1.f, -1.f, 0.f, 1.f}, {-1.f, 1.f, 0.f, 0.f}, {1.f, 1.f, 1.f, 0.f}, {-1.f, -1.f, 0.f, 1.f}, {1.f, 1.f, 1.f, 0.f}, {1.f, -1.f, 1.f, 1.f}};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(quad);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = quad;
    if (FAILED(d3d_->CreateBuffer(&bd, &sd, &vb_)))
        return false;

    D3D11_SAMPLER_DESC ss{};
    ss.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(d3d_->CreateSamplerState(&ss, &samp_)))
        return false;
    return true;
}

bool WinMFProvider::ensure_rt_and_pipeline(int w, int h)
{
    // RGBA RT
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_rgba_)))
        return false;
    if (FAILED(d3d_->CreateRenderTargetView(rt_rgba_.Get(), nullptr, &rtv_rgba_)))
        return false;

    // staging for readback
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.Usage = D3D11_USAGE_STAGING;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_stage_)))
        return false;

    // D2D target binding
    ComPtr<IDXGISurface> surf;
    rt_rgba_.As(&surf);
    D2D1_BITMAP_PROPERTIES1 bp = {};
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bp.dpiX = 96.f;
    bp.dpiY = 96.f;
    bp.bitmapOptions = (D2D1_BITMAP_OPTIONS)(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
    bp.colorContext = nullptr;

    if (FAILED(d2d_ctx_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &d2d_bitmap_rt_)))
    {
        DBG("D2D CreateBitmapFromDxgiSurface", E_FAIL);
        return false;
    }

    d2d_ctx_->SetTarget(d2d_bitmap_rt_.Get());

    return create_shaders_and_states();
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
WinMFProvider::createSRV_NV12(ID3D11Device *dev, ID3D11Texture2D *tex, bool uv)
{
    using Microsoft::WRL::ComPtr;
    if (!dev || !tex)
        return {};

    D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = 1;
    d.Format = uv ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;

    ComPtr<ID3D11ShaderResourceView> s;
    if (FAILED(dev->CreateShaderResourceView(tex, &d, &s)))
        return {};
    return s;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
WinMFProvider::createSRV_P010(ID3D11Device *dev, ID3D11Texture2D *tex, bool uv)
{
    using Microsoft::WRL::ComPtr;
    if (!dev || !tex)
        return {};

    D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = 1;
    d.Format = uv ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM;

    ComPtr<ID3D11ShaderResourceView> s;
    if (FAILED(dev->CreateShaderResourceView(tex, &d, &s)))
        return {};
    return s;
}

bool WinMFProvider::render_yuv_to_rgba(ID3D11Texture2D *yuvTex)
{
    if (!yuvTex)
        return false;

    // Create plane SRVs
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvY, srvUV;
    if (cur_subtype_ == MFVideoFormat_NV12)
    {
        srvY = createSRV_NV12(d3d_.Get(), yuvTex, false); // Y
        srvUV = createSRV_NV12(d3d_.Get(), yuvTex, true); // UV
    }
    else if (cur_subtype_ == MFVideoFormat_P010)
    {
        srvY = createSRV_P010(d3d_.Get(), yuvTex, false); // Y
        srvUV = createSRV_P010(d3d_.Get(), yuvTex, true); // UV
    }
    else
    {
        return false;
    }
    if (!srvY || !srvUV)
        return false;

    if (use_compute_nv12_ && cur_subtype_ == MFVideoFormat_NV12)
    {
        return render_nv12_to_rgba_cs(srvY.Get(), srvUV.Get());
    }

    // Set pipeline
    UINT stride = sizeof(float) * 4, offset = 0;
    ID3D11Buffer *pVB = vb_.Get();
    ctx_->IASetVertexBuffers(0, 1, &pVB, &stride, &offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(il_.Get());
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);

    ID3D11PixelShader *ps = (cur_subtype_ == MFVideoFormat_NV12) ? ps_nv12_.Get() : ps_p010_.Get();
    ctx_->PSSetShader(ps, nullptr, 0);

    ID3D11ShaderResourceView *srvs[2] = {srvY.Get(), srvUV.Get()};
    ctx_->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState *ss = samp_.Get();
    ctx_->PSSetSamplers(0, 1, &ss);

    float clear[4] = {0, 0, 0, 1};
    ID3D11RenderTargetView *rtv = rtv_rgba_.Get();
    // 設定 Viewport，否則 Draw 會跑在「沒有 viewport」的狀態下，畫面是 undefined（現在你看到的黑畫面＋D3D11 WARNING）
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<FLOAT>(cur_w_);
    vp.Height = static_cast<FLOAT>(cur_h_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->ClearRenderTargetView(rtv, clear);
    ctx_->Draw(6, 0);

    // unbind SRVs (avoid hazard)
    ID3D11ShaderResourceView *nulls[2] = {nullptr, nullptr};
    ctx_->PSSetShaderResources(0, 2, nulls);
    return true;
}

bool WinMFProvider::gpu_overlay_text(const wchar_t *text)
{
    if (!text || !*text)
        return true;

    d2d_ctx_->BeginDraw();
    d2d_ctx_->SetTransform(D2D1::Matrix3x2F::Identity());

    // 半透明黑底
    D2D1_RECT_F bg = D2D1::RectF(8.f, 8.f, 8.f + 520.f, 8.f + 28.f);
    d2d_ctx_->FillRectangle(bg, d2d_black_.Get());

    // 文字樣式
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;

    HRESULT hr = dwrite_->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, // ← 修正：SEMI_BOLD
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        16.0f,
        L"en-us",
        &fmt);
    if (FAILED(hr))
    {
        d2d_ctx_->EndDraw();
        return false;
    }

    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    D2D1_RECT_F rc = D2D1::RectF(12.f, 10.f, (float)cur_w_ - 12.f, 40.f);

    // 修正：ID2D1DeviceContext/RenderTarget 的方法叫 DrawText（不是 DrawTextW）
    d2d_ctx_->DrawText(
        text,
        (UINT32)wcslen(text),
        fmt.Get(),
        rc,
        d2d_white_.Get());

    return SUCCEEDED(d2d_ctx_->EndDraw());
}

// -------------------- Capture loop --------------------

void WinMFProvider::loop()
{
    while (running_)
    {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample);
        if (FAILED(hr))
        {
            emit_error(GCAP_EIO, "ReadSample failed");
            break;
        }
        if (!sample)
            continue;

        if (cpu_path_)
        {
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(&buf)))
                continue;

            BYTE *pData = nullptr;
            DWORD maxLen = 0, curLen = 0;
            if (FAILED(buf->Lock(&pData, &maxLen, &curLen)))
                continue;

            gcap_frame_t f{};
            f.width = cur_w_;
            f.height = cur_h_;
            f.pts_ns = (uint64_t)ts * 100;
            f.frame_id = ++frame_id_;

            if (cur_subtype_ == MFVideoFormat_ARGB32)
            {
                f.format = GCAP_FMT_ARGB;
                f.data[0] = pData;
                f.stride[0] = cur_w_ * 4;
                f.plane_count = 1;
                if (vcb_)
                    vcb_(&f, user_);
            }
            else if (cur_subtype_ == MFVideoFormat_NV12)
            {
                // NV12: Y 面在前，UV 在後
                int yStride = cur_w_;
                int uvStride = cur_w_;
                const uint8_t *y = pData;
                const uint8_t *uv = pData + yStride * cur_h_;

                // 轉成 ARGB32 臨時緩衝（確保大小）
                if (cpu_argb_.size() < (size_t)(cur_w_ * cur_h_ * 4))
                    cpu_argb_.assign(cur_w_ * cur_h_ * 4, 0);

                gcap::nv12_to_argb(y, uv, cur_w_, cur_h_, yStride, uvStride,
                                   cpu_argb_.data(), cur_w_ * 4);

                f.format = GCAP_FMT_ARGB;
                f.data[0] = cpu_argb_.data();
                f.stride[0] = cur_w_ * 4;
                f.plane_count = 1;
                if (vcb_)
                    vcb_(&f, user_);
            }
            else if (cur_subtype_ == MFVideoFormat_YUY2)
            {
                // YUY2 → ARGB32
                int yuy2Stride = cur_w_ * 2;
                const uint8_t *yuy2 = pData;

                if (cpu_argb_.size() < (size_t)(cur_w_ * cur_h_ * 4))
                    cpu_argb_.assign(cur_w_ * cur_h_ * 4, 0);

                gcap::yuy2_to_argb(yuy2, cur_w_, cur_h_, yuy2Stride,
                                   cpu_argb_.data(), cur_w_ * 4);

                f.format = GCAP_FMT_ARGB;
                f.data[0] = cpu_argb_.data();
                f.stride[0] = cur_w_ * 4;
                f.plane_count = 1;
                if (vcb_)
                    vcb_(&f, user_);
            }
            // 其他（例如 MJPG）理論上 VP 會幫我們解到 NV12/ARGB 之一；萬一還是 MJPG，可再加一個軟解（先不做）

            buf->Unlock();
            continue;
        }

        // 每一筆 sample 都會有 ts (100ns 單位)；我們前面用 f.pts_ns = ts * 100
        double fps_now = 0.0;
        if (last_pts_ns_ != 0)
        {
            uint64_t delta = ((uint64_t)ts * 100) - last_pts_ns_; // ns
            if (delta > 0)
                fps_now = 1e9 / (double)delta;
        }
        // 簡單一階濾波
        if (fps_now > 0.0)
        {
            if (fps_avg_ <= 0.0)
                fps_avg_ = fps_now;
            else
                fps_avg_ = fps_avg_ * 0.9 + fps_now * 0.1;
        }

        last_pts_ns_ = (uint64_t)ts * 100;

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf)))
            continue;

        // --- 嘗試：優先吃 IMFDXGIBuffer；沒有就自己 Upload ---
        ComPtr<IMFDXGIBuffer> dxgibuf;
        HRESULT hrDX = buf.As(&dxgibuf);

        ComPtr<ID3D11Texture2D> yuvTex;

        if (SUCCEEDED(hrDX) && dxgibuf)
        {
            // 裝置真的有 DXGI surface → 直接拿 GPU texture
            UINT subres = 0;
            if (FAILED(dxgibuf->GetResource(IID_PPV_ARGS(&yuvTex))))
            {
                DBG("DXGI: GetResource from IMFDXGIBuffer failed", E_FAIL);
                continue;
            }
            dxgibuf->GetSubresourceIndex(&subres);
        }
        else
        {
            // ★ 沒有 IMFDXGIBuffer：從 CPU NV12/P010 buffer Upload 到 D3D11 texture，再走 shader
            BYTE *pData = nullptr;
            DWORD maxLen = 0, curLen = 0;
            if (FAILED(buf->Lock(&pData, &maxLen, &curLen)))
                continue;

            if (!ensure_upload_yuv(cur_w_, cur_h_))
            {
                buf->Unlock();
                continue;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hrMap = ctx_->Map(upload_yuv_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(hrMap))
            {
                buf->Unlock();
                DBG("DXGI: Map(upload_yuv_) failed", hrMap);
                continue;
            }

            int w = cur_w_;
            int h = cur_h_;

            if (cur_subtype_ == MFVideoFormat_NV12)
            {
                // NV12: Y 平面 + 交錯 UV，這裡先假設 stride == width
                const uint8_t *srcY = pData;
                const uint8_t *srcUV = pData + w * h;
                uint8_t *dst = static_cast<uint8_t *>(mapped.pData);

                // Y plane
                for (int y = 0; y < h; ++y)
                    memcpy(dst + mapped.RowPitch * y,
                           srcY + w * y,
                           w);
                // UV plane (h/2 行，pitch 相同)
                for (int y = 0; y < h / 2; ++y)
                    memcpy(dst + mapped.RowPitch * (h + y),
                           srcUV + w * y,
                           w);
            }
            else if (cur_subtype_ == MFVideoFormat_P010)
            {
                // P010: 10-bit，2 bytes per sample
                const uint8_t *srcY = pData;
                const uint8_t *srcUV = pData + w * h * 2;
                uint8_t *dst = static_cast<uint8_t *>(mapped.pData);
                size_t rowBytes = (size_t)w * 2;

                for (int y = 0; y < h; ++y)
                    memcpy(dst + mapped.RowPitch * y,
                           srcY + rowBytes * y,
                           rowBytes);
                for (int y = 0; y < h / 2; ++y)
                    memcpy(dst + mapped.RowPitch * (h + y),
                           srcUV + rowBytes * y,
                           rowBytes);
            }

            ctx_->Unmap(upload_yuv_.Get(), 0);
            buf->Unlock();

            yuvTex = upload_yuv_.Get();
        }

        if (!yuvTex)
            continue;

        if (!render_yuv_to_rgba(yuvTex.Get()))
        {
            DBG("DXGI: render_yuv_to_rgba failed", E_FAIL);
            continue;
        }

        // ★後面原本的 GPU overlay / CopyResource / Map / vcb_ 全部保留

        // GPU overlay
        wchar_t wdev[256] = L"";
        if (!dev_name_.empty())
        {
            // 簡單從 UTF-8 轉 wchar_t 只為顯示（寬鬆作法）
            int wn = MultiByteToWideChar(CP_UTF8, 0, dev_name_.c_str(), -1, nullptr, 0);
            if (wn > 1 && wn <= 256)
            {
                MultiByteToWideChar(CP_UTF8, 0, dev_name_.c_str(), -1, wdev, 256);
            }
        }

        const wchar_t *fmtName =
            (cur_subtype_ == MFVideoFormat_P010)     ? L"P010"
            : (cur_subtype_ == MFVideoFormat_NV12)   ? L"NV12"
            : (cur_subtype_ == MFVideoFormat_ARGB32) ? L"ARGB32"
                                                     : L"?";

        const wchar_t *bitDepth =
            (cur_subtype_ == MFVideoFormat_P010) ? L"10-bit"
                                                 : L"8-bit"; // NV12 / ARGB32 皆 8-bit

        double fps_show = fps_avg_ > 0.0 ? fps_avg_ : 0.0;

        wchar_t line[512];
        swprintf(line, 512, L"%s | %dx%d @ %.2f fps | %s %s | #%llu",
                 (wdev[0] ? wdev : L"Device"),
                 cur_w_, cur_h_,
                 fps_show,
                 fmtName, bitDepth,
                 (unsigned long long)frame_id_);
        gpu_overlay_text(line);

        // Readback -> staging -> map
        ctx_->CopyResource(rt_stage_.Get(), rt_rgba_.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(rt_stage_.Get(), 0, D3D11_MAP_READ, 0, &m)))
        {
            gcap_frame_t f{};
            f.data[0] = m.pData;
            f.stride[0] = (int)m.RowPitch;
            f.plane_count = 1;
            f.width = cur_w_;
            f.height = cur_h_;
            f.format = GCAP_FMT_ARGB;
            f.pts_ns = (uint64_t)ts * 100;
            f.frame_id = ++frame_id_;
            if (vcb_)
                vcb_(&f, user_);
            ctx_->Unmap(rt_stage_.Get(), 0);
        }
    }
}

bool WinMFProvider::ensure_upload_yuv(int w, int h)
{
    if (!d3d_)
        return false;

    if (upload_yuv_)
    {
        D3D11_TEXTURE2D_DESC desc{};
        upload_yuv_->GetDesc(&desc);
        if ((int)desc.Width == w && (int)desc.Height == h)
            return true; // 尺寸相同就重複使用
        upload_yuv_.Reset();
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Format = (cur_subtype_ == MFVideoFormat_P010) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    td.Usage = D3D11_USAGE_DYNAMIC;             // 可 CPU 寫入
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // 給 pixel shader 當 SRV 用
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; // CPU write
    td.MiscFlags = 0;

    HRESULT hr = d3d_->CreateTexture2D(&td, nullptr, &upload_yuv_);
    if (FAILED(hr))
    {
        DBG("DXGI: Create upload NV12 texture failed", hr);
        return false;
    }
    return true;
}

// 建立/快取 Compute Shader 與 constant buffer
bool WinMFProvider::ensure_compute_shader()
{
    if (cs_nv12_)
        return true;
    if (!d3d_)
        return false;

    ComPtr<ID3DBlob> csb;
    ComPtr<ID3DBlob> err;

    HRESULT hr = D3DCompile(
        g_cs_nv12,
        strlen(g_cs_nv12),
        nullptr,
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        0,
        0,
        &csb,
        &err);
    if (FAILED(hr))
    {
        if (err)
            OutputDebugStringA((const char *)err->GetBufferPointer());
        DBG("DXGI: D3DCompile(g_cs_nv12) failed", hr);
        return false;
    }

    hr = d3d_->CreateComputeShader(
        csb->GetBufferPointer(),
        csb->GetBufferSize(),
        nullptr,
        &cs_nv12_);
    if (FAILED(hr))
    {
        DBG("DXGI: CreateComputeShader(cs_nv12_) failed", hr);
        return false;
    }

    // constant buffer：存 width/height
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(uint32_t) * 4;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = d3d_->CreateBuffer(&bd, nullptr, &cs_params_);
    if (FAILED(hr))
    {
        DBG("DXGI: CreateBuffer(cs_params_) failed", hr);
        return false;
    }

    return true;
}

// 用 Compute Shader 把 NV12 轉成 rt_rgba_（RGBA8）
bool WinMFProvider::render_nv12_to_rgba_cs(ID3D11ShaderResourceView *srvY,
                                           ID3D11ShaderResourceView *srvUV)
{
    if (!srvY || !srvUV || !rt_rgba_ || !ctx_)
        return false;
    if (!ensure_compute_shader())
        return false;

    // 建立/快取 rt_rgba_ 對應的 UAV
    if (!rt_uav_)
    {
        // 讓 D3D 幫我們用這張貼圖本來的格式建立 UAV
        HRESULT hr = d3d_->CreateUnorderedAccessView(rt_rgba_.Get(), nullptr, &rt_uav_);
        if (FAILED(hr))
        {
            DBG("DXGI: CreateUnorderedAccessView(rt_rgba_) failed", hr);
            return false;
        }
    }

    // 更新 constant buffer（寬、高）
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hrMap = ctx_->Map(cs_params_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hrMap))
    {
        DBG("DXGI: Map(cs_params_) failed", hrMap);
        return false;
    }
    auto *p = reinterpret_cast<uint32_t *>(mapped.pData);
    p[0] = static_cast<uint32_t>(cur_w_);
    p[1] = static_cast<uint32_t>(cur_h_);
    ctx_->Unmap(cs_params_.Get(), 0);

    ID3D11ShaderResourceView *srvs[2] = {srvY, srvUV};
    ID3D11UnorderedAccessView *uavs[1] = {rt_uav_.Get()};
    ID3D11Buffer *cbs[1] = {cs_params_.Get()};

    ctx_->CSSetShader(cs_nv12_.Get(), nullptr, 0);
    ctx_->CSSetShaderResources(0, 2, srvs);
    ctx_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx_->CSSetConstantBuffers(0, 1, cbs);

    UINT gx = (cur_w_ + 15) / 16;
    UINT gy = (cur_h_ + 15) / 16;
    ctx_->Dispatch(gx, gy, 1);

    // 清掉 CS 綁定，避免影響其他地方
    ID3D11ShaderResourceView *nullSrvs[2] = {nullptr, nullptr};
    ID3D11UnorderedAccessView *nullUavs[1] = {nullptr};
    ID3D11Buffer *nullCb[1] = {nullptr};

    ctx_->CSSetShader(nullptr, nullptr, 0);
    ctx_->CSSetShaderResources(0, 2, nullSrvs);
    ctx_->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    ctx_->CSSetConstantBuffers(0, 1, nullCb);

    return true;
}
