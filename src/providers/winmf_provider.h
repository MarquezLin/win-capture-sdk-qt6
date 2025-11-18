#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <mutex>

#include "gcapture.h"
#include "../core/capture_manager.h"

// Media Foundation
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

// D3D / DXGI / D2D / DWrite
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <d3dcompiler.h>

#include <wrl.h>
using Microsoft::WRL::ComPtr;

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
    // ---- Callbacks ----
    gcap_on_video_cb vcb_ = nullptr;
    gcap_on_error_cb ecb_ = nullptr;
    void *user_ = nullptr;

    // ---- State ----
    std::atomic<bool> running_{false};
    std::thread th_;
    uint64_t frame_id_ = 0;
    std::string dev_name_; // 目前選用的裝置名稱（UTF-8）

    // ---- MF objects ----
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFSourceReader> reader_;

    // Requested profile (hint)
    gcap_profile_t profile_{1920, 1080, 60, 1, GCAP_FMT_NV12};

    // Negotiated native output (kept as NV12 or P010)
    int cur_w_ = 0;
    int cur_h_ = 0;
    int cur_stride_ = 0;
    GUID cur_subtype_ = GUID_NULL; // MFVideoFormat_NV12 or MFVideoFormat_P010

    // ---- D3D11 / DXGI ----
    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11Device1> d3d1_;
    ComPtr<ID3D11DeviceContext1> ctx1_;

    ComPtr<IMFDXGIDeviceManager> dxgi_mgr_;
    UINT dxgi_token_ = 0;

    // Render target (RGBA8) + staging for readback
    ComPtr<ID3D11Texture2D> rt_rgba_;
    ComPtr<ID3D11RenderTargetView> rtv_rgba_;
    ComPtr<ID3D11Texture2D> rt_stage_;

    // Pipeline resources
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_nv12_;
    ComPtr<ID3D11PixelShader> ps_p010_;
    ComPtr<ID3D11InputLayout> il_;
    ComPtr<ID3D11Buffer> vb_;
    ComPtr<ID3D11SamplerState> samp_;

    // D2D/DWrite for GPU text overlay
    ComPtr<ID2D1Factory1> d2d_factory_;
    ComPtr<ID2D1Device> d2d_device_;
    ComPtr<ID2D1DeviceContext> d2d_ctx_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1SolidColorBrush> d2d_white_;
    ComPtr<ID2D1SolidColorBrush> d2d_black_;
    ComPtr<ID2D1Bitmap1> d2d_bitmap_rt_;

    // --- internal helpers ---
    void loop();
    void emit_error(gcap_status_t c, const char *msg);

    // init
    bool create_d3d();
    bool create_reader_with_dxgi(int devIndex);
    bool pick_best_native(GUID &sub, UINT32 &w, UINT32 &h, UINT32 &fn, UINT32 &fd);

    // rendering
    bool ensure_rt_and_pipeline(int w, int h);
    bool create_shaders_and_states();
    bool render_yuv_to_rgba(ID3D11Texture2D *yuvTex);
    bool gpu_overlay_text(const wchar_t *text);

    // utils
    static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
    createSRV_NV12(ID3D11Device *dev, ID3D11Texture2D *tex, bool uv);

    static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
    createSRV_P010(ID3D11Device *dev, ID3D11Texture2D *tex, bool uv);

    bool use_dxgi_ = false;
    bool cpu_path_ = true;

    bool create_reader_cpu_only(int devIndex);

    std::vector<uint8_t> cpu_argb_;
};
