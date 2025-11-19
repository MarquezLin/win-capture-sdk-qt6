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

// ============================================================
// DirectShow backend for Windows capture.
// 這個版本先專心把 DirectShow provider 的骨架做好、能編譯。
// 之後我們再一步一步加上 SampleGrabber + GPU shader。
// ============================================================

class DShowProvider : public ICaptureProvider
{
public:
    DShowProvider();
    ~DShowProvider() override;

    // ---- ICaptureProvider 介面 ----
    bool enumerate(std::vector<gcap_device_info_t> &list) override;
    bool open(int index) override;
    bool setProfile(const gcap_profile_t &p) override;
    bool setBuffers(int count, size_t bytes_hint) override;
    bool start() override;
    void stop() override;
    void close() override;
    void setCallbacks(gcap_on_video_cb vcb, gcap_on_error_cb ecb, void *user) override;

    // 之後如果要用 SampleGrabber callback，可以在這裡進來
    void onSample(double sampleTime, BYTE *data, long len);

private:
    void ensure_com();
    void uninit_com();

    bool buildGraphForDevice(int index);

private:
    // DirectShow graph 相關
    Microsoft::WRL::ComPtr<IGraphBuilder> graph_;
    Microsoft::WRL::ComPtr<IMediaControl> mediaControl_;
    Microsoft::WRL::ComPtr<IMediaEvent> mediaEvent_;
    Microsoft::WRL::ComPtr<IBaseFilter> sourceFilter_;

    int currentIndex_ = -1;
    std::atomic<bool> running_{false};

    // 設定與 callback
    gcap_profile_t profile_{};
    gcap_on_video_cb vcb_ = nullptr;
    gcap_on_error_cb ecb_ = nullptr;
    void *user_ = nullptr;

    std::mutex mtx_;
    bool comInited_ = false;
};

// ============================================================
// 這裡先不在 .h 裡定義 CLSID / IID，避免跟 .cpp 重複。
// 之後真的要用 SampleGrabber 時，我們只在 .cpp 裡用 DEFINE_GUID。
// ============================================================
