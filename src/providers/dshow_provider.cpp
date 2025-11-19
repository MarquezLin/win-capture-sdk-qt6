// src/providers/dshow_provider.cpp
#include "dshow_provider.h"
#include <objbase.h>
#include <strsafe.h>

using Microsoft::WRL::ComPtr;

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
    if (!comInited_)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr))
            comInited_ = true;
    }
}

void DShowProvider::uninit_com()
{
    if (comInited_)
    {
        CoUninitialize();
        comInited_ = false;
    }
}

void DShowProvider::setCallbacks(gcap_on_video_cb vcb, gcap_on_error_cb ecb, void *user)
{
    std::lock_guard<std::mutex> lock(mtx_);
    vcb_ = vcb;
    ecb_ = ecb;
    user_ = user;
}

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
    {
        // 沒有裝置或失敗
        return false;
    }

    ComPtr<IMoniker> moniker;
    ULONG fetched = 0;
    int index = 0;

    while (enumMoniker->Next(1, &moniker, &fetched) == S_OK)
    {
        ComPtr<IPropertyBag> propBag;
        hr = moniker->BindToStorage(0, 0, IID_PPV_ARGS(&propBag));
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
                int len = WideCharToMultiByte(CP_UTF8, 0, varName.bstrVal, -1,
                                              nameBuf, sizeof(nameBuf), nullptr, nullptr);
                if (len <= 0)
                {
                    StringCchPrintfA(nameBuf, sizeof(nameBuf), "DShowDevice%d", index);
                }

                strncpy_s(di.name, nameBuf, sizeof(di.name) - 1);
                di.symbolic_link[0] = '\0';
                di.caps = 0;

                list.push_back(di);
                ++index;
            }

            VariantClear(&varName);
        }

        moniker.Reset();
    }

    return !list.empty();
}

bool DShowProvider::buildGraphForDevice(int index)
{
    graph_.Reset();
    mediaControl_.Reset();
    mediaEvent_.Reset();
    sourceFilter_.Reset();

    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&graph_));
    if (FAILED(hr))
        return false;

    hr = graph_.As(&mediaControl_);
    if (FAILED(hr))
        return false;

    hr = graph_.As(&mediaEvent_);
    if (FAILED(hr))
        return false;

    // 重新 enumerate 找 index 對應的 moniker
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

    // ※ 這裡暫時不接 SampleGrabber / Renderer
    return true;
}

bool DShowProvider::open(int index)
{
    ensure_com();
    std::lock_guard<std::mutex> lock(mtx_);

    close();

    if (!buildGraphForDevice(index))
        return false;

    currentIndex_ = index;
    return true;
}

bool DShowProvider::setProfile(const gcap_profile_t &p)
{
    std::lock_guard<std::mutex> lock(mtx_);
    profile_ = p;
    // 暫時不做實際的 IAMStreamConfig 設定
    return true;
}

bool DShowProvider::setBuffers(int, size_t)
{
    // DirectShow 有自己的 buffer 管理，這裡先忽略
    return true;
}

bool DShowProvider::start()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!graph_ || !mediaControl_)
        return false;

    HRESULT hr = mediaControl_->Run();
    if (FAILED(hr))
    {
        if (ecb_)
            ecb_(GCAP_EIO, "DShowProvider: Run() failed", user_);
        return false;
    }

    running_ = true;

    // 目前沒有接 SampleGrabber，所以不會有畫面、只是把 graph 跑起來。
    if (ecb_)
        ecb_(GCAP_ENOTSUP, "DShowProvider: start() skeleton only (no frames yet)", user_);

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
    std::lock_guard<std::mutex> lock(mtx_);
    stop();

    sourceFilter_.Reset();
    mediaEvent_.Reset();
    mediaControl_.Reset();
    graph_.Reset();

    currentIndex_ = -1;
}

void DShowProvider::onSample(double /*sampleTime*/, BYTE * /*data*/, long /*len*/)
{
    // 之後接 SampleGrabber 時會用到，目前留空
}
