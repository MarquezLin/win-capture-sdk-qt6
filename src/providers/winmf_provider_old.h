#pragma once
#include <vector>
#include <thread>
#include <atomic>
#include <wrl/client.h>
#include <mfapi.h>       // Media Foundation API
#include <mfidl.h>       // Defines IMFMediaSource and other Media Foundation COM interfaces
#include <mfreadwrite.h> // IMFSourceReader, IMFMediaSink ...
#include <mferror.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include "../core/capture_manager.h"

// WinMFProvider implements the ICaptureProvider interface
// It provides a capture backend using Microsoft Media Foundation (MF).
class WinMFProvider : public ICaptureProvider
{
public:
    WinMFProvider();           // Constructor - initializes Media Foundation
    ~WinMFProvider() override; // Destructor - stops and releases resources

    // Enumerate available video capture devices
    bool enumerate(std::vector<gcap_device_info_t> &list) override;

    // Open a capture device by index
    bool open(int index) override;

    // Set the capture profile (resolution, fps, pixel format)
    bool setProfile(const gcap_profile_t &p) override;

    // Set number of buffers and size hints (unused here)
    bool setBuffers(int count, size_t bytes_hint) override;

    // Start capture loop (spawns a background thread)
    bool start() override;

    // Stop the capture loop and join the background thread
    void stop() override;

    // Close and release the device and reader
    void close() override;

    // Set callback functions for video frames and errors
    void setCallbacks(gcap_on_video_cb vcb, gcap_on_error_cb ecb, void *user) override;

private:
    // COM smart pointers to Media Foundation objects
    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    Microsoft::WRL::ComPtr<IMFMediaSource> source_;

    // Capture configuration (default = 1080p60 NV12)
    gcap_profile_t profile_{1920, 1080, 60, 1, GCAP_FMT_NV12};

    // Actual output info from the reader (after format conversion)
    int cur_w_ = 0;
    int cur_h_ = 0;
    int cur_stride_ = 0;
    GUID cur_subtype_ = GUID_NULL;

    // Callback pointers
    gcap_on_video_cb vcb_ = nullptr;
    gcap_on_error_cb ecb_ = nullptr;
    void *user_ = nullptr;

    // Worker thread control
    std::thread th_;
    std::atomic<bool> running_{false};
    uint64_t frame_id_ = 0;

    // Internal helper methods
    bool configureReader(int devIndex);                // Setup IMFSourceReader for the selected device
    void loop();                                       // Frame capture loop
    void emit_error(gcap_status_t c, const char *msg); // Centralized error reporting

private:
    // ---- Media Foundation ----
    Microsoft::WRL::ComPtr<IMFMediaSource> source_;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;

    // ---- Negotiated output info (native NV12/P010 kept) ----
    int cur_w_ = 0, cur_h_ = 0;
    GUID cur_subtype_ = GUID_NULL; // MFVideoFormat_NV12 or MFVideoFormat_P010

    // ---- D3D11 / DXGI ----
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgi_mgr_;
    UINT dxgi_token_ = 0;

    // Render target (RGBA8) + staging for readback
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt_rgba_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_rgba_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt_stage_; // CPU readback

    // Shaders / states
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_nv12_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_p010_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> il_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samp_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_; // fullscreen quad

    // Direct2D/DirectWrite for GPU overlay
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2d_ctx_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> d2d_brush_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> d2d_bitmap_rt_; // bound to rt_rgba_

    bool create_d3d();
    bool create_reader_with_dxgi(int devIndex);
    bool pick_best_native(GUID &sub_out, UINT32 &w, UINT32 &h, UINT32 &fn, UINT32 &fd);
    bool ensure_rt_and_pipeline(int w, int h);
    bool render_yuv_to_rgba(ID3D11Texture2D *yuvTex, UINT planeIndex = 0);
    bool gpu_overlay_text(const wchar_t *text);
};
