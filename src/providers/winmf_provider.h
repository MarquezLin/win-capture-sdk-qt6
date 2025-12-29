#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include <deque>

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
#include <string>
using Microsoft::WRL::ComPtr;

class WinMFProvider : public ICaptureProvider
{
public:
    // WinMFProvider();           // Constructor - initializes Media Foundation
    explicit WinMFProvider(bool preferGpu = true);
    ~WinMFProvider() override; // Destructor - stops and releases resources

    // 讓外部指定要用哪一張 D3D11 Adapter（DXGI EnumAdapters1 的 index）
    // index = -1 表示「使用系統預設 adapter」
    static void setPreferredAdapterIndex(int index);

    // Enumerate available video capture devices
    bool enumerate(std::vector<gcap_device_info_t> &list) override;

    // Open a capture device by index
    bool open(int index) override;

    // Set the capture profile (resolution, fps, pixel format)
    bool setProfile(const gcap_profile_t &p) override;

    // ---- Recording control (NV12 → H.264, P010 → HEVC) ----
    gcap_status_t startRecording(const char *pathUtf8);
    gcap_status_t stopRecording();

    // Select WASAPI capture endpoint for recording audio.
    // device_id_utf8 from gcap_enumerate_audio_devices; nullptr/"" => use default endpoint.
    gcap_status_t setRecordingAudioDevice(const char *device_id_utf8);

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

    bool getDeviceProps(gcap_device_props_t &out) override;
    bool getSignalStatus(gcap_signal_status_t &out) override;
    bool setProcessing(const gcap_processing_opts_t &opts) override;

    bool isUsingGpu() const { return use_dxgi_ && !cpu_path_; }

private:
    // ---- Callbacks ----
    gcap_on_video_cb vcb_ = nullptr;
    gcap_on_error_cb ecb_ = nullptr;
    void *user_ = nullptr;

    std::mutex pending_mtx_;
    std::deque<std::string> pending_logs_;
    static constexpr size_t kMaxPendingLogs = 256;

    // ---- State ----
    std::atomic<bool> running_{false};
    std::thread th_;
    uint64_t frame_id_ = 0;
    std::string dev_name_;        // 目前選用的裝置名稱（UTF-8）
    std::wstring dev_sym_link_w_; // MF device symbolic link（給 SetupAPI 查 Driver/FW/Serial 用）
    double fps_avg_ = 0.0;
    uint64_t last_pts_ns_ = 0;
    bool use_dxgi_ = false;
    bool cpu_path_ = true;

    // ---- MF objects ----
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFSourceReader> reader_;

    // Requested profile (hint)
    gcap_profile_t profile_{1920, 1080, 60, 1, GCAP_FMT_NV12};

    // Negotiated native output (kept as NV12 or P010)
    int cur_w_ = 0;
    int cur_h_ = 0;
    int cur_fps_num_ = 0;
    int cur_fps_den_ = 1;
    int cur_stride_ = 0;
    GUID cur_subtype_ = GUID_NULL; // MFVideoFormat_NV12 or MFVideoFormat_P010 or MFVideoFormat_YUY2

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

    // CPU→GPU 上傳用的 NV12 / P010 / YUY2  texture
    Microsoft::WRL::ComPtr<ID3D11Texture2D> upload_yuv_;
    // CPU→GPU 上傳用的 YUY2（打包成 RGBA8_UINT，寬度 = ceil(w/2)）
    Microsoft::WRL::ComPtr<ID3D11Texture2D> upload_yuy2_packed_;

    // +++ Compute shader (NV12 → RGBA) + UAV for output
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs_nv12_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cs_params_;           // 存 width/height
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> rt_uav_; // rt_rgba_ 對應的 UAV

    // 是否啟用 NV12 的 compute 路徑（之後你也可以拉成 UI 開關）
    bool use_compute_nv12_ = true; // 先預設開啟

    // Pipeline resources
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_nv12_;
    ComPtr<ID3D11PixelShader> ps_p010_;
    ComPtr<ID3D11PixelShader> ps_yuy2_;
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
    // open() 階段 callbacks 尚未設好時，先把 GCAP_OK 類型 log 暫存起來
    void pending_log_push(const char *msg);
    void pending_log_flush();

    // init
    bool create_d3d();
    bool create_reader_with_dxgi(int devIndex);
    bool pick_best_native(GUID &sub, UINT32 &w, UINT32 &h, UINT32 &fn, UINT32 &fd);

    // rendering
    bool ensure_rt_and_pipeline(int w, int h);
    bool create_shaders_and_states();
    bool render_yuv_to_rgba(ID3D11Texture2D *yuvTex);
    bool gpu_overlay_text(const wchar_t *text);

    // 確保 upload_yuv_ 尺寸 / format 正確
    bool ensure_upload_yuv(int w, int h);
    // 確保 upload_yuy2_packed_ 尺寸 / format 正確（寬度 = ceil(w/2)）
    bool ensure_upload_yuy2_packed(int w, int h);

    // Compute shader 相關 helper
    bool ensure_compute_shader();
    bool render_nv12_to_rgba_cs(ID3D11ShaderResourceView *srvY,
                                ID3D11ShaderResourceView *srvUV);

    // utils
    static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
    createSRV_NV12(ID3D11Device *dev, ID3D11Texture2D *tex, bool uv);

    static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
    createSRV_P010(ID3D11Device *dev, ID3D11Texture2D *tex, bool uv);

    bool create_reader_cpu_only(int devIndex);

    // ---- Recording (Media Foundation Sink Writer) ----
    struct MfRecorder;
    std::unique_ptr<MfRecorder> recorder_;
    std::mutex recorderMutex_;
    // Recording audio endpoint id (WASAPI endpoint id, UTF-8). Empty => system default.
    std::string rec_audio_device_id_;

    std::vector<uint8_t> cpu_argb_;

    bool prefer_gpu_ = true;

    // ---- GPU（D3D Adapter）相關 ----
    // 從 DXGI 取得的 GPU 名稱（用在浮水印顯示）
    std::wstring gpu_name_w_;

    // 全域：目前偏好的 Adapter index（由 UI/C API 設定）
    static std::atomic<int> s_adapter_index_;
};
