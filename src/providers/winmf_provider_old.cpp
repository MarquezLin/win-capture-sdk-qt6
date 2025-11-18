#include "winmf_provider.h"
#include <vector>
#include <string>
#include <chrono>
#include <cstring>
#include <mfobjects.h>   // IMFAttributes, IMFMediaType...
#include <combaseapi.h>  // CoInitializeEx, CoTaskMemFree...
#include <propvarutil.h> // PROPVARIANT tool
#include "../core/frame_converter.h"

using Microsoft::WRL::ComPtr;

// Link necessary Windows libraries for Media Foundation
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")

// Global flag to ensure Media Foundation is initialized once
static bool g_mfInited = false;

// Initialize Media Foundation only once per process
static void ensure_mf()
{
    if (!g_mfInited)
    {
        MFStartup(MF_VERSION);
        g_mfInited = true;
    }
}

static std::string to_utf8(const wchar_t *w)
{
    if (!w)
        return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return {};
    std::string s(bytes - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), bytes, nullptr, nullptr);
    return s;
}

// Create an IMFMediaType for the requested configuration (Video + Subtype + Width/Height + FPS)
static HRESULT BuildRequestedVideoType(IMFMediaType **ppType,
                                       const GUID &subtype,
                                       int width, int height,
                                       int fps_num, int fps_den)
{
    if (!ppType)
        return E_POINTER;
    *ppType = nullptr;

    ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr))
        return hr;

    hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr))
        return hr;

    hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (FAILED(hr))
        return hr;

    hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
    if (FAILED(hr))
        return hr;

    hr = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, (UINT32)fps_num, (UINT32)fps_den);
    if (FAILED(hr))
        return hr;

    *ppType = type.Detach();
    return S_OK;
}

// Iterate through a list of subtypes; if one succeeds, return true and apply it to the reader
static bool TrySetTypeInOrder(IMFSourceReader *reader,
                              const GUID *subtypes, size_t count,
                              int width, int height, int fps_num, int fps_den,
                              GUID *chosen)
{
    for (size_t i = 0; i < count; ++i)
    {
        ComPtr<IMFMediaType> req;
        if (FAILED(BuildRequestedVideoType(&req, subtypes[i], width, height, fps_num, fps_den)))
            continue;

        HRESULT hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, req.Get());
        if (SUCCEEDED(hr))
        {
            if (chosen)
                *chosen = subtypes[i];
            return true;
        }
    }
    return false;
}

// Constructor - initialize Media Foundation
WinMFProvider::WinMFProvider() { ensure_mf(); }

// Destructor - stop capture and release resources
WinMFProvider::~WinMFProvider()
{
    stop();
    close();
}

// Emit error callback if provided
void WinMFProvider::emit_error(gcap_status_t c, const char *msg)
{
    if (ecb_)
        ecb_(c, msg, user_);
}

// Enumerate available video capture devices using Media Foundation
bool WinMFProvider::enumerate(std::vector<gcap_device_info_t> &list)
{
    ensure_mf();
    // Create an IMFAttributes object and reserve one attribute slot(capacity = 1)
    ComPtr<IMFAttributes> attr;

    // if it fails, return false to indicate enumeration filed (typically due to an environment issue or an API call failure)
    if (FAILED(MFCreateAttributes(&attr, 1)))
        return false;
    // Set the filter criteria(enumerate video capture sources)
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    // Based on the attributes provided by attr,
    // scan all Media Foundation device sources in the system and list all media devices that match the conditions.
    IMFActivate **pp = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attr.Get(), &pp, &count)))
        return false;

    // Clear the user-provided list to prevent leftover data from previous operations.
    list.clear();

    for (UINT32 i = 0; i < count; ++i)
    {
        WCHAR *wname = nullptr;
        UINT32 cch = 0;
        gcap_device_info_t di{};
        di.index = (int)i;
        di.caps = 0;

        // Get Friendly Name
        if (SUCCEEDED(pp[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &wname, &cch)))
        {
            std::string name = to_utf8(wname);
            std::snprintf(di.name, sizeof(di.name), "%s", name.c_str());
            CoTaskMemFree(wname);
        }
        else
        {
            std::snprintf(di.name, sizeof(di.name), "VideoDevice_%u", i);
        }

        // get symbolic link for unique ID
        WCHAR *wlink = nullptr;
        if (SUCCEEDED(pp[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &wlink, &cch)))
        {
            // You could store it somewhere
            std::string symbolic_link = to_utf8(wlink);
            std::snprintf(di.symbolic_link, sizeof(di.symbolic_link), "%s", symbolic_link.c_str());
            CoTaskMemFree(wlink);
        }

        list.push_back(di);
        pp[i]->Release();
    }

    CoTaskMemFree(pp);
    return true;
}

bool WinMFProvider::open(int index)
{
    return configureReader(index);
}

bool WinMFProvider::setProfile(const gcap_profile_t &p)
{
    profile_ = p;
    return true;
}

bool WinMFProvider::setBuffers(int, size_t) { return true; }

bool WinMFProvider::start()
{
    if (!reader_)
        return false;
    running_ = true;
    th_ = std::thread([this]
                      { loop(); });
    return true;
}

void WinMFProvider::stop()
{
    running_ = false;
    if (th_.joinable())
        th_.join();
}

void WinMFProvider::close()
{
    source_.Reset();
    reader_.Reset();
}

void WinMFProvider::setCallbacks(gcap_on_video_cb v, gcap_on_error_cb e, void *u)
{
    vcb_ = v;
    ecb_ = e;
    user_ = u;
}

bool WinMFProvider::create_d3d()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // 給 D2D/DirectWrite
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 fls, _countof(fls), D3D11_SDK_VERSION,
                                 &d3d_, &got, &ctx_)))
        return false;

    // DXGI Device Manager 給 MF
    if (FAILED(MFCreateDXGIDeviceManager(&dxgi_token_, &dxgi_mgr_)))
        return false;
    Microsoft::WRL::ComPtr<IDXGIDevice> xdev;
    d3d_.As(&xdev);
    if (FAILED(dxgi_mgr_->ResetDevice(xdev.Get(), dxgi_token_)))
        return false;

    // D2D / DWrite for overlay
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.ReleaseAndGetAddressOf())))
        return false;
    Microsoft::WRL::ComPtr<ID2D1Device> d2ddev;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
    d3d_.As(&dxgiDev);
    d2d_factory_->CreateDevice(dxgiDev.Get(), &d2d_device_);
    d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_ctx_);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &dwrite_);
    return true;
}

bool WinMFProvider::create_reader_with_dxgi(int devIndex)
{
    // enumerate & Activate IMFMediaSource 省略（你原本的就好）

    // 建 SourceReader 屬性（關鍵三個）
    Microsoft::WRL::ComPtr<IMFAttributes> rdAttr;
    MFCreateAttributes(&rdAttr, 3);
    rdAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    rdAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    rdAttr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgi_mgr_.Get());

    return SUCCEEDED(MFCreateSourceReaderFromMediaSource(source_.Get(), rdAttr.Get(),
                                                         reader_.ReleaseAndGetAddressOf()));
}

bool WinMFProvider::pick_best_native(GUID &sub, UINT32 &w, UINT32 &h, UINT32 &fn, UINT32 &fd)
{
    struct Cand
    {
        GUID sub;
        UINT32 w, h, fn, fd;
        int score;
        Microsoft::WRL::ComPtr<IMFMediaType> mt;
    };
    std::vector<Cand> cds;
    for (DWORD i = 0;; ++i)
    {
        Microsoft::WRL::ComPtr<IMFMediaType> t;
        HRESULT hr = reader_->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t);
        if (hr == MF_E_NO_MORE_TYPES)
            break;
        if (FAILED(hr) || !t)
            continue;

        GUID major = GUID_NULL, s = GUID_NULL;
        if (FAILED(t->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video)
            continue;
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &s)))
            continue;
        if (!(s == MFVideoFormat_P010 || s == MFVideoFormat_NV12))
            continue; // 只收 P010 / NV12

        UINT32 cw = 0, ch = 0;
        UINT32 fnum = 0, fden = 1;
        if (FAILED(MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &cw, &ch)))
            continue;
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &fnum, &fden);

        auto pref = [&](const GUID &g)
        { return (g == MFVideoFormat_P010) ? 2 : 1; };
        long long area = (long long)cw * ch;
        double fps = fden ? (double)fnum / fden : 0.0;
        int score = (int)(area / 1000) * 1000 + (int)(fps * 10) * 10 + pref(s); // 面積>fps>格式
        cds.push_back({s, cw, ch, fnum, fden, score, t});
    }
    if (cds.empty())
        return false;
    std::sort(cds.begin(), cds.end(), [](auto &a, auto &b)
              { return a.score > b.score; });
    if (FAILED(reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, cds[0].mt.Get())))
        return false;
    sub = cds[0].sub;
    w = cds[0].w;
    h = cds[0].h;
    fn = cds[0].fn;
    fd = cds[0].fd;
    return true;
}

bool WinMFProvider::ensure_rt_and_pipeline(int w, int h)
{
    // RGBA8 Render Target
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // 便於 D2D
    td.SampleDesc = {1, 0};
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_rgba_)))
        return false;
    if (FAILED(d3d_->CreateRenderTargetView(rt_rgba_.Get(), nullptr, &rtv_rgba_)))
        return false;

    // Staging (CPU readback)
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.Usage = D3D11_USAGE_STAGING;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_stage_)))
        return false;

    // Quad / Sampler / Shaders（HLSL 編譯結果綁進來；此處略去完整編譯流程）
    // vs_：全螢幕三角/矩形頂點著色器
    // ps_nv12_ / ps_p010_：下面給你 HLSL 片段
    // samp_：LINEAR + CLAMP
    // vb_ / il_：全螢幕三角形頂點（two-tri 或 single-tri）

    // Direct2D 綁定 render target（用 DXGI surface）
    Microsoft::WRL::ComPtr<IDXGISurface> surf;
    rt_rgba_.As(&surf);
    D2D1_BITMAP_PROPERTIES1 bp = {
        {DXGI_FORMAT_B8G8R8A8_UNORM, 96.f, 96.f},
        D2D1_ALPHA_MODE_PREMULTIPLIED,
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        0};
    if (FAILED(d2d_ctx_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &d2d_bitmap_rt_)))
        return false;
    d2d_ctx_->SetTarget(d2d_bitmap_rt_.Get());
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &d2d_brush_);
    return true;
}

bool WinMFProvider::configureReader(int devIndex)
{
    ensure_mf();

    // 2.1 建 D3D11 + DXGI Manager
    if (!create_d3d())
    {
        emit_error(GCAP_EIO, "D3D11 init failed");
        return false;
    }

    // 2.2 用 DXGI Manager 建 SourceReader（啟用硬體轉換 + VideoProcessing）
    if (!create_reader_with_dxgi(devIndex))
    {
        emit_error(GCAP_EIO, "Create reader failed");
        return false;
    }

    // 2.3 挑「最高解析度」的 native 模式（偏好 P010→NV12），並保持原生格式（不轉 ARGB32）
    UINT32 w = 0, h = 0, fn = 0, fd = 1;
    GUID sub = GUID_NULL;
    if (!pick_best_native(sub, w, h, fn, fd))
    {
        emit_error(GCAP_EINVAL, "No native format");
        return false;
    }
    cur_subtype_ = sub;
    cur_w_ = (int)w;
    cur_h_ = (int)h;

    // 2.4 建立 RGBA RenderTarget + 管線（shader/sampler/quad），大小照談成的 w×h
    if (!ensure_rt_and_pipeline(cur_w_, cur_h_))
    {
        emit_error(GCAP_EIO, "Create render target failed");
        return false;
    }

    // 2.5 僅開第一條視訊
    reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    return true;
}

bool WinMFProvider::gpu_overlay_text(const wchar_t *text)
{
    d2d_ctx_->BeginDraw();
    d2d_ctx_->SetTransform(D2D1::Matrix3x2F::Identity());
    auto rect = D2D1::RectF(10.f, 10.f, (float)cur_w_ - 10.f, 60.f);

    // 背景半透明
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bg;
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.55f), &bg);
    d2d_ctx_->FillRectangle(D2D1::RectF(6, 6, 6 + 520, 6 + 28), bg.Get());

    // 文字
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMIBOLD,
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              16.0f, L"en-us", &fmt);
    d2d_ctx_->DrawTextW(text, (UINT32)wcslen(text), fmt.Get(), rect, d2d_brush_.Get());
    return SUCCEEDED(d2d_ctx_->EndDraw());
}

void WinMFProvider::loop()
{
    while (running_)
    {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        if (FAILED(reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample)))
        {
            emit_error(GCAP_EIO, "ReadSample failed");
            break;
        }
        if (!sample)
            continue;

        // 取 DXGI 紋理
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf)))
            continue;

        Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgibuf;
        if (FAILED(buf.As(&dxgibuf)))
            continue;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> yuvTex;
        UINT subres = 0;
        if (FAILED(dxgibuf->GetResource(IID_PPV_ARGS(&yuvTex))))
            continue;
        dxgibuf->GetSubresourceIndex(&subres);

        // GPU: YUV→RGBA 到 rt_rgba_
        if (!render_yuv_to_rgba(yuvTex.Get()))
            continue;

        // GPU: 疊字（解析度/FPS/裝置名等）
        gpu_overlay_text(L"Demo | NV12/P010 via GPU | 1920x1080 @ 59.94");

        // Readback: 複製到 staging，Map 取出指標，丟回 Qt（與你現有 GUI 相容）
        ctx_->CopyResource(rt_stage_.Get(), rt_rgba_.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(rt_stage_.Get(), 0, D3D11_MAP_READ, 0, &m)))
        {
            // 組 gcap_frame_t（data[0]=m.pData, stride[0]=m.RowPitch, format=GCAP_FMT_ARGB）
            // 你現有的 s_vcb() 會 copy，所以 Unmap 之後安全
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

// // Create a Media Foundation source (IMFMediaSource)
// // and a corresponding IMFSourceReader from the specified video capture device (devIndex).
// // Then configure the reader stream with the desired resolution,
// // frame rate, and pixel format (default: NV12), and finally enable only the first video stream.
// bool WinMFProvider::configureReader(int devIndex)
// {
//     ensure_mf();

//     // Create an IMFAttributes object and reserve one attribute slot(capacity = 1)
//     ComPtr<IMFAttributes> attr;

//     // if it fails, return false to indicate enumeration filed (typically due to an environment issue or an API call failure)
//     if (FAILED(MFCreateAttributes(&attr, 1)))
//         return false;
//     // Set the filter criteria(enumerate video capture sources)
//     attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

//     // Based on the attributes provided by attr,
//     // scan all Media Foundation device sources in the system and list all media devices that match the conditions.
//     IMFActivate **pp = nullptr;
//     UINT32 count = 0;
//     if (FAILED(MFEnumDeviceSources(attr.Get(), &pp, &count)))
//         return false;
//     if ((UINT32)devIndex >= count)
//     {
//         for (UINT32 i = 0; i < count; ++i)
//             pp[i]->Release();
//         CoTaskMemFree(pp);
//         return false;
//     }

//     // Activate the selected IMFActivate at index devIndex into a real IMFMediaSource object, and store it in the member source_
//     // __uuidof(IMFMediaSource): tells the system which interface type you want to obtain.
//     // ReleaseAndGetAddressOf(): releases the current pointer held by source_,
//     // then obtains the address where a new pointer can be stored by the COM function.
//     if (FAILED(pp[devIndex]->ActivateObject(__uuidof(IMFMediaSource), (void **)source_.ReleaseAndGetAddressOf())))
//     {
//         for (UINT32 i = 0; i < count; ++i)
//             pp[i]->Release();
//         CoTaskMemFree(pp);
//         return false;
//     }
//     for (UINT32 i = 0; i < count; ++i)
//         pp[i]->Release();
//     CoTaskMemFree(pp);

//     // Create an attribute object for the reader and enable video processing
//     ComPtr<IMFAttributes> rdAttr;
//     MFCreateAttributes(&rdAttr, 2);
//     rdAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
//     // Generate an IMFSourceReader from the IMFMediaSource — this serves as the entry point for subsequent ReadSample or Read calls.
//     MFCreateSourceReaderFromMediaSource(source_.Get(), rdAttr.Get(), &reader_);

//     static const GUID kPreferredSubtypes[] = {
//         MFVideoFormat_NV12, // 8-bit, 4:2:0
//         MFVideoFormat_YUY2, // 8-bit, 4:2:2
//         MFVideoFormat_MJPG, // Compressed MJPEG
//         MFVideoFormat_P010, // 10-bit 4:2:0 (N12)
//         MFVideoFormat_v210  // 10-bit 4:2:2
//     };
//     GUID chosen = GUID_NULL;
//     bool ok = TrySetTypeInOrder(
//         reader_.Get(),
//         kPreferredSubtypes, _countof(kPreferredSubtypes),
//         profile_.width, profile_.height,
//         profile_.fps_num, profile_.fps_den,
//         &chosen);

//     if (!ok)
//     {
//         // Enumerate the native media types and select the one that’s the closest match
//         // (at least the resolution must match; apply it even if the FPS differs).
//         DWORD i = 0;
//         for (;; ++i)
//         {
//             ComPtr<IMFMediaType> nat;
//             HRESULT hr = reader_->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &nat);
//             if (hr == MF_E_NO_MORE_TYPES)
//                 break;
//             if (FAILED(hr) || !nat)
//                 continue;

//             GUID major = GUID_NULL, sub = GUID_NULL;
//             if (FAILED(nat->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video)
//                 continue;
//             if (FAILED(nat->GetGUID(MF_MT_SUBTYPE, &sub)))
//                 continue;

//             UINT32 w = 0, h = 0;
//             if (FAILED(MFGetAttributeSize(nat.Get(), MF_MT_FRAME_SIZE, &w, &h)))
//                 continue;

//             // First, lock onto media types with an exact resolution match (the FPS doesn’t have to match exactly)
//             if (w == (UINT32)profile_.width && h == (UINT32)profile_.height)
//             {
//                 // Use the device-provided native type directly (the safest option) without modifying any attributes.
//                 hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nat.Get());
//                 if (SUCCEEDED(hr))
//                 {
//                     chosen = sub;
//                     ok = true;
//                     break;
//                 }
//             }
//         }
//     }

//     if (!ok)
//     {
//         // specify only the Subtype without providing Width/Height or FPS, allowing the source to select the default configuration automatically.
//         // (Use the first subtype from the preference list that succeeds.)
//         for (size_t i = 0; i < _countof(kPreferredSubtypes); ++i)
//         {
//             ComPtr<IMFMediaType> t;
//             if (FAILED(MFCreateMediaType(&t)))
//                 continue;
//             if (FAILED(t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)))
//                 continue;
//             if (FAILED(t->SetGUID(MF_MT_SUBTYPE, kPreferredSubtypes[i])))
//                 continue;

//             HRESULT hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.Get());
//             if (SUCCEEDED(hr))
//             {
//                 chosen = kPreferredSubtypes[i];
//                 ok = true;
//                 break;
//             }
//         }
//     }

//     if (!ok)
//     {
//         emit_error(GCAP_EINVAL, "No acceptable media type (NV12/YUY2/MJPG/P010/v210) at requested size/fps");
//         return false;
//     }

// // fixed N12
// #if 0
//     // Create a desired media type (IMFMediaType).
//     ComPtr<IMFMediaType> type;
//     MFCreateMediaType(&type);
//     type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
//     // Set the major type to video and the subtype to NV12
//     type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
//     // Configure the image resolution and frame rate using the values from your member variable profile_
//     MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, profile_.width, profile_.height);
//     MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, profile_.fps_num, profile_.fps_den);
//     // Attempt to configure the first video stream with the media type you just specified.
//     if (FAILED(reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get())))
//     {
//         emit_error(GCAP_EINVAL, "SetCurrentMediaType failed");
//         return false;
//     }
// #endif

//     // --- Request ARGB32 as the final output from SourceReader ---
//     Microsoft::WRL::ComPtr<IMFMediaType> out;
//     if (SUCCEEDED(BuildRequestedVideoType(&out, MFVideoFormat_ARGB32,
//                                           profile_.width, profile_.height,
//                                           profile_.fps_num, profile_.fps_den)))
//     {
//         // Let MF do the format conversion (MJPEG/YUY2/NV12 -> ARGB32)
//         HRESULT hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get());
//         if (FAILED(hr))
//         {
//             emit_error(GCAP_EINVAL, "SetCurrentMediaType ARGB32 failed");
//             return false;
//         }
//     }

//     // Read back the actual negotiated media type (size / stride / subtype)
//     Microsoft::WRL::ComPtr<IMFMediaType> cur;
//     if (SUCCEEDED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)))
//     {
//         UINT32 w = 0, h = 0;
//         MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
//         cur_w_ = (int)w;
//         cur_h_ = (int)h;
//         if (FAILED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32 *)&cur_stride_)) || cur_stride_ <= 0)
//             cur_stride_ = cur_w_ * 4; // ARGB32 fallback
//         cur->GetGUID(MF_MT_SUBTYPE, &cur_subtype_);
//     }

//     // First, disable all streams (including audio and any additional video streams).
//     reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
//     // Then, enable only the first video stream to avoid capturing unrelated audio or secondary video sources.
//     reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
//     return true;
// }

// void WinMFProvider::loop()
// {
//     while (running_)
//     {
//         DWORD stream = 0, flags = 0;
//         LONGLONG ts = 0;
//         ComPtr<IMFSample> sample;
//         HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample);
//         if (FAILED(hr))
//         {
//             emit_error(GCAP_EIO, "ReadSample failed");
//             break;
//         }
//         if (!sample)
//             continue;

//         ComPtr<IMFMediaBuffer> buf;
//         if (FAILED(sample->ConvertToContiguousBuffer(&buf)))
//             continue;

//         BYTE *pData = nullptr;
//         DWORD maxLen = 0, curLen = 0;
//         if (FAILED(buf->Lock(&pData, &maxLen, &curLen)))
//             continue;

//         // // NV12 layout: Y plane + interleaved UV
//         // int w = profile_.width, h = profile_.height;
//         // int yStride = w; // 通常等於寬度；嚴格來說應讀 MT_DEFAULT_STRIDE
//         // const uint8_t *y = pData;
//         // const uint8_t *uv = pData + (size_t)yStride * h;

//         // // 轉成 BGRA 方便 Qt 顯示
//         // std::vector<uint8_t> bgra((size_t)w * h * 4);
//         // gcap::nv12_to_argb(y, uv, w, h, yStride, yStride, bgra.data(), w * 4);

//         // gcap_frame_t f{};
//         // f.data[0] = bgra.data();

//         // Reader 已輸出 ARGB32，直接用實際寬高/stride 丟給上層
//         const int w = (cur_w_ > 0) ? cur_w_ : profile_.width;
//         const int h = (cur_h_ > 0) ? cur_h_ : profile_.height;
//         const int s = (cur_stride_ > 0) ? cur_stride_ : (w * 4);

//         gcap_frame_t f{};
//         f.data[0] = pData; // 注意：上層會立刻 copy
//         f.stride[0] = s;
//         f.plane_count = 1;
//         f.width = w;
//         f.height = h;
//         f.format = GCAP_FMT_ARGB;
//         f.pts_ns = (uint64_t)(ts * 100); // 100ns → ns
//         f.frame_id = ++frame_id_;

//         if (vcb_)
//             vcb_(&f, user_);

//         buf->Unlock();
//     }
// }
