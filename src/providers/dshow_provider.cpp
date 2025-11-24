#include "dshow_provider.h"
#include <objbase.h>
#include <strsafe.h>
#include <initguid.h>
#include <dvdmedia.h> // MEDIASUBTYPE_NV12 / YUY2
#include "../core/frame_converter.h"

using Microsoft::WRL::ComPtr;

// 放在匿名 namespace 裡，做全域一次性的 COM 初始化
namespace
{
    void global_com_init()
    {
        static std::once_flag flag;
        std::call_once(flag, []
                       {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            // 和 WinMF 一樣處理：S_OK / S_FALSE / RPC_E_CHANGED_MODE 都當成 OK，用不到就只 log
            if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE)
            {
                OutputDebugStringA("[DShow] CoInitializeEx failed\n");
            } });
    }
}

// ------------------------------------------------------------
// SampleGrabber / NullRenderer CLSID / IID 定義
// ------------------------------------------------------------
DEFINE_GUID(CLSID_SampleGrabber,
            0xc1f400a0, 0x3f08, 0x11d3,
            0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37);

DEFINE_GUID(IID_ISampleGrabber,
            0x6b652fff, 0x11fe, 0x4fce,
            0x92, 0xad, 0x02, 0x66, 0xb5, 0xd7, 0xc8, 0x07);

DEFINE_GUID(IID_ISampleGrabberCB,
            0x0579154a, 0x2b53, 0x4994,
            0xb0, 0x5f, 0x8f, 0xf8, 0x6c, 0xa0, 0x00, 0x08);

DEFINE_GUID(CLSID_NullRenderer,
            0xc1f400a4, 0x3f08, 0x11d3,
            0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37);

// ============================================================
// SampleGrabberCBImpl
// ============================================================

SampleGrabberCBImpl::SampleGrabberCBImpl(DShowProvider *owner)
    : owner_(owner)
{
}

STDMETHODIMP SampleGrabberCBImpl::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB)
    {
        *ppv = static_cast<ISampleGrabberCB *>(this);
        AddRef();
        return S_OK;
    }

    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG)
SampleGrabberCBImpl::AddRef()
{
    return ++refCount_;
}

STDMETHODIMP_(ULONG)
SampleGrabberCBImpl::Release()
{
    ULONG r = --refCount_;
    if (r == 0)
        delete this;
    return r;
}

STDMETHODIMP SampleGrabberCBImpl::BufferCB(double sampleTime, BYTE *buffer, long len)
{
    if (owner_)
        owner_->onSample(sampleTime, buffer, len);
    return S_OK;
}

// ============================================================
// DShowProvider
// ============================================================

DShowProvider::DShowProvider()
{
    ensure_com();
}

DShowProvider::~DShowProvider()
{
    stop();
    close();
    uninit_com();
}

void DShowProvider::ensure_com()
{
    global_com_init();
}

void DShowProvider::uninit_com()
{
    // 不再呼叫 CoUninitialize()，交給系統在 process 結束時清理
}

void DShowProvider::setCallbacks(gcap_on_video_cb vcb, gcap_on_error_cb ecb, void *user)
{
    std::lock_guard<std::mutex> lock(mtx_);
    vcb_ = vcb;
    ecb_ = ecb;
    user_ = user;
}

// Enumerate devices
bool DShowProvider::enumerate(std::vector<gcap_device_info_t> &list)
{
    ensure_com();
    list.clear();

    ComPtr<ICreateDevEnum> devEnum;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&devEnum));
    if (FAILED(hr))
        return false;

    ComPtr<IEnumMoniker> enumMoniker;
    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
    if (hr != S_OK)
        return false;

    ComPtr<IMoniker> moniker;
    ULONG fetched = 0;
    int index = 0;

    while (enumMoniker->Next(1, &moniker, &fetched) == S_OK)
    {
        ComPtr<IPropertyBag> propBag;
        hr = moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&propBag));
        if (SUCCEEDED(hr))
        {
            VARIANT varName;
            VariantInit(&varName);

            hr = propBag->Read(L"FriendlyName", &varName, nullptr);
            if (SUCCEEDED(hr))
            {
                gcap_device_info_t di{};
                di.index = index;

                char nameBuf[128] = {};
                WideCharToMultiByte(CP_UTF8, 0,
                                    varName.bstrVal, -1,
                                    nameBuf, sizeof(nameBuf),
                                    nullptr, nullptr);
                strncpy_s(di.name, nameBuf, sizeof(di.name) - 1);
                di.caps = 0;
                di.symbolic_link[0] = '\0';

                list.push_back(di);
                ++index;
            }

            VariantClear(&varName);
        }

        moniker.Reset();
    }

    return !list.empty();
}

// Build graph: Source -> SampleGrabber -> NullRenderer
bool DShowProvider::buildGraphForDevice(int index)
{
    graph_.Reset();
    mediaControl_.Reset();
    mediaEvent_.Reset();
    sourceFilter_.Reset();
    grabberFilter_.Reset();
    nullRenderer_.Reset();
    width_ = height_ = 0;
    subtype_ = MEDIASUBTYPE_NULL;

    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&graph_));
    if (FAILED(hr))
        return false;

    graph_.As(&mediaControl_);
    graph_.As(&mediaEvent_);

    // find device by index
    ComPtr<ICreateDevEnum> devEnum;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&devEnum));
    if (FAILED(hr))
        return false;

    ComPtr<IEnumMoniker> enumMoniker;
    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
    if (hr != S_OK)
        return false;

    ComPtr<IMoniker> moniker;
    ULONG fetched = 0;
    int cur = 0;
    while (enumMoniker->Next(1, &moniker, &fetched) == S_OK)
    {
        if (cur == index)
            break;
        moniker.Reset();
        ++cur;
    }
    if (!moniker)
        return false;

    hr = moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&sourceFilter_));
    if (FAILED(hr))
        return false;

    hr = graph_->AddFilter(sourceFilter_.Get(), L"VideoCapture");
    if (FAILED(hr))
        return false;

    // SampleGrabber
    ComPtr<ISampleGrabber> grabber;
    hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ISampleGrabber, (void **)&grabber);
    if (FAILED(hr))
        return false;

    grabber.As(&grabberFilter_);
    hr = graph_->AddFilter(grabberFilter_.Get(), L"SampleGrabber");
    if (FAILED(hr))
        return false;

    // NullRenderer
    hr = CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&nullRenderer_));
    if (FAILED(hr))
        return false;

    hr = graph_->AddFilter(nullRenderer_.Get(), L"NullRenderer");
    if (FAILED(hr))
        return false;

    // Set media type: prefer NV12, fallback YUY2
    AM_MEDIA_TYPE mt{};
    mt.majortype = MEDIATYPE_Video;
    mt.formattype = FORMAT_VideoInfo;
    mt.subtype = MEDIASUBTYPE_NV12;
    hr = grabber->SetMediaType(&mt);
    if (FAILED(hr))
    {
        mt.subtype = MEDIASUBTYPE_YUY2;
        grabber->SetMediaType(&mt);
    }

    // CaptureGraphBuilder2 to connect pins
    ComPtr<ICaptureGraphBuilder2> capBuilder;
    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&capBuilder));
    if (FAILED(hr))
        return false;

    capBuilder->SetFiltergraph(graph_.Get());

    hr = capBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                  sourceFilter_.Get(), grabberFilter_.Get(), nullRenderer_.Get());
    if (FAILED(hr))
        return false;

    // ---- NEW: set SampleGrabber callback here ----
    grabberFilter_.As(&grabber);
    if (!grabber)
        return false;

    SampleGrabberCBImpl *cb = new SampleGrabberCBImpl(this);
    grabber->SetCallback(cb, 1); // 1 = BufferCB
    cb->Release();

    // get connected media type to know real width/height/subtype
    AM_MEDIA_TYPE cmt{};
    ZeroMemory(&cmt, sizeof(cmt));
    if (SUCCEEDED(grabber->GetConnectedMediaType(&cmt)))
    {
        if (cmt.formattype == FORMAT_VideoInfo &&
            cmt.cbFormat >= sizeof(VIDEOINFOHEADER) &&
            cmt.pbFormat)
        {
            VIDEOINFOHEADER *vih = reinterpret_cast<VIDEOINFOHEADER *>(cmt.pbFormat);
            width_ = vih->bmiHeader.biWidth;
            height_ = std::abs(vih->bmiHeader.biHeight);
            subtype_ = cmt.subtype;
        }

        if (cmt.cbFormat && cmt.pbFormat)
            CoTaskMemFree(cmt.pbFormat);
        if (cmt.pUnk)
            cmt.pUnk->Release();
    }

    if (width_ <= 0 || height_ <= 0)
        return false;

    // configure SampleGrabber
    grabber->SetOneShot(FALSE);
    grabber->SetBufferSamples(FALSE);

    // callback 之後在 open() 設
    return true;
}

bool DShowProvider::open(int index)
{
    ensure_com();
    close();
    std::lock_guard<std::mutex> lock(mtx_);

    if (!buildGraphForDevice(index))
        return false;

    currentIndex_ = index;
    return true;
}

bool DShowProvider::setProfile(const gcap_profile_t &p)
{
    std::lock_guard<std::mutex> lock(mtx_);
    profile_ = p;
    // 目前未使用 IAMStreamConfig 強制設解析度，先吃預設
    return true;
}

bool DShowProvider::setBuffers(int, size_t)
{
    // DirectShow 內部控管 buffer，不需要特別處理
    return true;
}

bool DShowProvider::start()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!mediaControl_)
        return false;

    HRESULT hr = mediaControl_->Run();
    if (FAILED(hr))
    {
        if (ecb_)
            ecb_(GCAP_EIO, "DShow: Run() failed", user_);
        return false;
    }

    running_ = true;
    return true;
}

void DShowProvider::stop()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (mediaControl_ && running_)
    {
        mediaControl_->Stop();
        running_ = false;
    }
}

void DShowProvider::close()
{
    stop();

    std::lock_guard<std::mutex> lock(mtx_);

    nullRenderer_.Reset();
    grabberFilter_.Reset();
    sourceFilter_.Reset();
    mediaEvent_.Reset();
    mediaControl_.Reset();
    graph_.Reset();

    width_ = height_ = 0;
    subtype_ = MEDIASUBTYPE_NULL;
    currentIndex_ = -1;
}

// onSample: DirectShow thread → CPU convert → gcap_frame_t
void DShowProvider::onSample(double sampleTime, BYTE *data, long len)
{
    if (!running_)
        return;
    if (!data || len <= 0)
        return;

    char buf[128];
    sprintf_s(buf, "DShow onSample: t=%.3f, len=%ld\n", sampleTime, len);
    OutputDebugStringA(buf);

    // gcap_on_video_cb vcb = nullptr;
    // void *user = nullptr;
    // {
    //     std::lock_guard<std::mutex> lock(mtx_);
    //     vcb = vcb_;
    //     user = user_;
    // }
    // if (!vcb)
    //     return;

    // if (width_ <= 0 || height_ <= 0)
    //     return;

    // gcap_frame_t f{};
    // f.width = width_;
    // f.height = height_;
    // f.pts_ns = static_cast<uint64_t>(sampleTime * 1e9);
    // f.frame_id = 0; // TODO: 之後可以改成 atomic 計數
    // f.format = GCAP_FMT_ARGB;
    // f.plane_count = 1;

    // static std::vector<uint8_t> argb;
    // argb.resize(static_cast<size_t>(width_) * height_ * 4);

    // uint8_t *out = argb.data(); // 這個給轉換函式用

    // f.format = GCAP_FMT_ARGB;
    // f.data[0] = out; // 轉完的 buffer 指標
    // f.stride[0] = width_ * 4;
    // f.plane_count = 1;

    // if (subtype_ == MEDIASUBTYPE_NV12)
    // {
    //     int yStride = width_;
    //     int uvStride = width_;
    //     const uint8_t *y = reinterpret_cast<const uint8_t *>(data);
    //     const uint8_t *uv = y + yStride * height_;

    //     gcap::nv12_to_argb(
    //         y, uv,
    //         width_, height_,
    //         yStride, uvStride,
    //         out, // ★ 用 out，不要用 f.data[0]
    //         f.stride[0]);
    // }
    // else if (subtype_ == MEDIASUBTYPE_YUY2)
    // {
    //     int strideYUY2 = width_ * 2;
    //     const uint8_t *src = reinterpret_cast<const uint8_t *>(data);

    //     gcap::yuy2_to_argb(
    //         src,
    //         width_, height_,
    //         strideYUY2,
    //         out, // ★ 用 out，不要用 f.data[0]
    //         f.stride[0]);
    // }
    // else
    // {
    //     return; // 不支援其他格式
    // }

    // vcb(&f, user);
}
