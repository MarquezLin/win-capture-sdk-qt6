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

#define DBG(stage, hr)                                                                \
    do                                                                                \
    {                                                                                 \
        std::string __m = std::string("[WinMF] ") + stage + " failed: " + hr_msg(hr); \
        OutputDebugStringA((__m + "\n").c_str());                                     \
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

WinMFProvider::WinMFProvider() {}
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

    // 保險模式：強制 CPU 路徑，不建立任何 D3D/D2D
    use_dxgi_ = false;
    cpu_path_ = true;

    // 只建立最寬鬆的 reader（不帶任何屬性），保證可建
    if (!create_reader_cpu_only(index))
    {
        emit_error(GCAP_EIO, "Create reader (CPU) failed");
        OutputDebugStringA("[WinMF] open(): Create reader (CPU) failed\n");
        return false;
    }

    // 先嘗試 NV12
    {
        ComPtr<IMFMediaType> mt;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        HRESULT hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt.Get());
        if (FAILED(hr))
        {
            DBG("SetCurrentMediaType(NV12)", hr);
            // NV12 不行再試 ARGB32
            ComPtr<IMFMediaType> mt2;
            MFCreateMediaType(&mt2);
            mt2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            mt2->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
            hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt2.Get());
            if (FAILED(hr))
            {
                DBG("SetCurrentMediaType(ARGB32)", hr);
                // 兩個都失敗就不再硬設，直接讀回目前的實際型態
            }
        }
    }

    // 讀回實際談好的型態
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
        if (FAILED(hr))
            return hr;
        if ((UINT32)devIndex >= count)
        {
            for (UINT32 i = 0; i < count; ++i)
                pp[i]->Release();
            CoTaskMemFree(pp);
            return E_INVALIDARG;
        }
        hr = pp[devIndex]->ActivateObject(__uuidof(IMFMediaSource),
                                          (void **)out.ReleaseAndGetAddressOf());
        for (UINT32 i = 0; i < count; ++i)
            pp[i]->Release();
        CoTaskMemFree(pp);
        return hr;
    };

    HRESULT hr = S_OK;

    // Try #1: DXGI + VP + HW
    if (dxgi_mgr_)
    {
        hr = activate_source(source_);
        if (SUCCEEDED(hr))
        {
            ComPtr<IMFAttributes> rdAttr;
            hr = MFCreateAttributes(&rdAttr, 3);
            if (SUCCEEDED(hr))
            {
                rdAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
                rdAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
                rdAttr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgi_mgr_.Get());
                hr = MFCreateSourceReaderFromMediaSource(source_.Get(), rdAttr.Get(), &reader_);
            }
            if (SUCCEEDED(hr))
            {
                use_dxgi_ = true;
                cpu_path_ = false;
                return true;
            }
            else
            {
                DBG("CreateReader DXGI+VP+HW", hr);
                if (source_)
                    source_->Shutdown(); // 這顆已經不乾淨，丟掉
                source_.Reset();
                reader_.Reset();
            }
        }
        else
        {
            DBG("ActivateObject(IMFMediaSource)", hr);
        }
    }

    // Try #2: VP only（無 DXGI）
    hr = activate_source(source_);
    if (SUCCEEDED(hr))
    {
        ComPtr<IMFAttributes> rdAttr;
        hr = MFCreateAttributes(&rdAttr, 1);
        if (SUCCEEDED(hr))
        {
            rdAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            hr = MFCreateSourceReaderFromMediaSource(source_.Get(), rdAttr.Get(), &reader_);
        }
        if (SUCCEEDED(hr))
        {
            use_dxgi_ = false; // 無 DXGI
            return true;
        }
        else
        {
            DBG("CreateReader VP-only", hr);
            if (source_)
                source_->Shutdown();
            source_.Reset();
            reader_.Reset();
        }
    }
    else
    {
        DBG("ActivateObject(IMFMediaSource)", hr);
    }

    // Try #3: bare（最寬鬆，不帶任何屬性）
    hr = activate_source(source_);
    if (SUCCEEDED(hr))
    {
        hr = MFCreateSourceReaderFromMediaSource(source_.Get(), nullptr, &reader_);
        if (SUCCEEDED(hr))
        {
            use_dxgi_ = false;
            return true;
        }
        else
        {
            DBG("CreateReader bare", hr);
            if (source_)
                source_->Shutdown();
            source_.Reset();
            reader_.Reset();
        }
    }
    else
    {
        DBG("ActivateObject(IMFMediaSource)", hr);
    }

    return false;
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
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
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

        ComPtr<IMFDXGIBuffer> dxgibuf;
        if (FAILED(buf.As(&dxgibuf)))
            continue;

        ComPtr<ID3D11Texture2D> yuvTex;
        UINT subres = 0;
        if (FAILED(dxgibuf->GetResource(IID_PPV_ARGS(&yuvTex))))
            continue;
        dxgibuf->GetSubresourceIndex(&subres);

        if (!render_yuv_to_rgba(yuvTex.Get()))
            continue;

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
