// src/providers/dshow_provider.h
#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <wrl/client.h>

#include <windows.h>
#include <dshow.h>

#include "gcapture.h"
#include "../core/capture_manager.h"

// DirectShow CPU 版本（NV12 → YUY2 fallback）
// 之後 GPU 會接在這個架構上

class DShowProvider : public ICaptureProvider
{
public:
    DShowProvider();
    ~DShowProvider() override;

    bool enumerate(std::vector<gcap_device_info_t> &list) override;
    bool open(int index) override;
    bool setProfile(const gcap_profile_t &p) override;
    bool setBuffers(int count, size_t bytes_hint) override;
    bool start() override;
    void stop() override;
    void close() override;
    void setCallbacks(gcap_on_video_cb vcb, gcap_on_error_cb ecb, void *user) override;

    // SampleGrabber callback 進入點
    void onSample(double sampleTime, BYTE *data, long len);

private:
    void ensure_com();
    void uninit_com();

    bool buildGraphForDevice(int index);
    bool setupSampleGrabber();
    bool selectMediaType(); // NV12 → YUY2 fallback

private:
    Microsoft::WRL::ComPtr<IGraphBuilder> graph_;
    Microsoft::WRL::ComPtr<IMediaControl> mediaControl_;
    Microsoft::WRL::ComPtr<IMediaEvent> mediaEvent_;

    Microsoft::WRL::ComPtr<IBaseFilter> sourceFilter_;
    Microsoft::WRL::ComPtr<IBaseFilter> grabberFilter_;
    Microsoft::WRL::ComPtr<IBaseFilter> nullRenderer_;

    // 格式（NV12 or YUY2）
    GUID subtype_ = MEDIASUBTYPE_NULL;
    int width_ = 0;
    int height_ = 0;

    std::atomic<bool> running_{false};
    int currentIndex_ = -1;

    gcap_profile_t profile_{};
    gcap_on_video_cb vcb_ = nullptr;
    gcap_on_error_cb ecb_ = nullptr;
    void *user_ = nullptr;

    std::mutex mtx_;
    // bool comInited_ = false;
};

// -------- SampleGrabber interface（避免使用過期 qedit.h）--------

struct ISampleGrabberCB : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample *pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE *pBuffer, long BufferLen) = 0;
};

struct __declspec(uuid("6b652fff-11fe-4fce-92ad-0266b5d7c807")) ISampleGrabber : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long *pBufferSize, long *pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample **ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB *pCallback, long WhichMethodToCallback) = 0;
};

// ----- Callback 實作者 -----

class SampleGrabberCBImpl : public ISampleGrabberCB
{
public:
    explicit SampleGrabberCBImpl(DShowProvider *owner);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG)
    AddRef() override;
    STDMETHODIMP_(ULONG)
    Release() override;

    // ISampleGrabberCB
    STDMETHODIMP SampleCB(double, IMediaSample *) override { return S_OK; }
    STDMETHODIMP BufferCB(double sampleTime, BYTE *buffer, long len) override;

private:
    std::atomic<ULONG> refCount_{1};
    DShowProvider *owner_ = nullptr;
};
