// Avoid Windows min/max macros breaking std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "winmf_provider.h"
#include <mferror.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <sstream>
#include <comdef.h>  // for _com_error
#include <windows.h> // for OutputDebugStringA
#include <string>
#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
#include <condition_variable>

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <setupapi.h>
#include <devpkey.h>
#include <cmath>
namespace
{
    // DEVPROPKEY = { fmtid(GUID), pid }
    static const DEVPROPKEY kDevPKey_Device_DriverVersion = {
        {0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}},
        3};

    static const DEVPROPKEY kDevPKey_Device_FirmwareVersion = {
        {0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}},
        4};

    static const DEVPROPKEY kDevPKey_Device_SerialNumber = {
        {0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}},
        256};
}
#pragma comment(lib, "setupapi.lib")
#include "../core/frame_converter.h"

using Microsoft::WRL::ComPtr;

// WASAPI 需要 ole32
#pragma comment(lib, "ole32.lib")

static std::string hr_msg(HRESULT hr)
{
    _com_error ce(hr);
    std::ostringstream oss;
    oss << "hr=0x" << std::hex << std::uppercase << (unsigned long)hr
        << " (" << (ce.ErrorMessage() ? ce.ErrorMessage() : "unknown") << ")";
    return oss.str();
}

// --- local helper: wide string -> UTF-8 string ---
static std::string wide_to_utf8(const std::wstring &ws)
{
    if (ws.empty())
        return {};

    const int len = WideCharToMultiByte(CP_UTF8, 0,
                                        ws.c_str(), -1,
                                        nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 0)
        return {};

    std::string out((size_t)len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0,
                        ws.c_str(), -1,
                        out.data(), len,
                        nullptr, nullptr);
    return out;
}

// --- compatibility: 讓你檔案裡原本的 utf8_from_wide(...) 全部不用改 ---
static std::string utf8_from_wide(const std::wstring &ws)
{
    return wide_to_utf8(ws);
}

static gcap_pixfmt_t mfsub_to_gcap(const GUID &sub)
{
    if (sub == MFVideoFormat_NV12)
        return GCAP_FMT_NV12;
    if (sub == MFVideoFormat_YUY2)
        return GCAP_FMT_YUY2;
    if (sub == MFVideoFormat_P010)
        return GCAP_FMT_P010;
    if (sub == MFVideoFormat_ARGB32)
        return GCAP_FMT_ARGB;
    return GCAP_FMT_ARGB; // fallback（你也可改成 NV12）
}

static int pixfmt_bitdepth(gcap_pixfmt_t f)
{
    switch (f)
    {
    case GCAP_FMT_P010:
    case GCAP_FMT_R210:
    case GCAP_FMT_V210:
        return 10;
    default:
        return 8;
    }
}

static std::wstring get_mf_string(IMFActivate *act, const GUID &key)
{
    if (!act)
        return {};
    wchar_t *w = nullptr;
    UINT32 cch = 0;
    if (FAILED(act->GetAllocatedString(key, &w, &cch)) || !w)
        return {};
    std::wstring s(w);
    CoTaskMemFree(w);
    return s;
}

static bool setupapi_open_by_interface(const std::wstring &symLink, HDEVINFO &outSet, SP_DEVINFO_DATA &outDevInfo)
{
    outSet = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (outSet == INVALID_HANDLE_VALUE)
        return false;

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);
    if (!SetupDiOpenDeviceInterfaceW(outSet, symLink.c_str(), 0, &ifData))
        return false;

    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(outSet, &ifData, nullptr, 0, &required, nullptr);
    if (required == 0)
        return false;

    std::vector<uint8_t> buf(required);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    outDevInfo = {};
    outDevInfo.cbSize = sizeof(outDevInfo);
    if (!SetupDiGetDeviceInterfaceDetailW(outSet, &ifData, detail, required, nullptr, &outDevInfo))
        return false;

    return true;
}

static std::wstring setupapi_get_prop_string(HDEVINFO set, SP_DEVINFO_DATA &devInfo, const DEVPROPKEY &key)
{
    DEVPROPTYPE type = 0;
    DWORD bytes = 0;
    SetupDiGetDevicePropertyW(set, &devInfo, &key, &type, nullptr, 0, &bytes, 0);
    if (bytes == 0)
        return {};
    std::vector<uint8_t> buf(bytes);
    if (!SetupDiGetDevicePropertyW(set, &devInfo, &key, &type, buf.data(), (DWORD)buf.size(), &bytes, 0))
        return {};
    if (type != DEVPROP_TYPE_STRING)
        return {};
    return std::wstring(reinterpret_cast<wchar_t *>(buf.data()));
}

bool WinMFProvider::getDeviceProps(gcap_device_props_t &out)
{
    memset(&out, 0, sizeof(out));

    strncpy_s(out.driver_version, sizeof(out.driver_version), "Unknown", _TRUNCATE);
    strncpy_s(out.firmware_version, sizeof(out.firmware_version), "Unknown", _TRUNCATE);
    strncpy_s(out.serial_number, sizeof(out.serial_number), "Unknown", _TRUNCATE);

    if (dev_sym_link_w_.empty())
        return true;

    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA devInfo{};
    if (!setupapi_open_by_interface(dev_sym_link_w_, set, devInfo))
    {
        if (set != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(set);
        return true;
    }

    std::wstring drvVer =
        setupapi_get_prop_string(set, devInfo, kDevPKey_Device_DriverVersion);
    std::wstring fwVer =
        setupapi_get_prop_string(set, devInfo, kDevPKey_Device_FirmwareVersion);
    std::wstring sn =
        setupapi_get_prop_string(set, devInfo, kDevPKey_Device_SerialNumber);

    if (!drvVer.empty())
    {
        auto s = wide_to_utf8(drvVer);
        strncpy_s(out.driver_version, sizeof(out.driver_version), s.c_str(), _TRUNCATE);
    }

    if (!fwVer.empty())
    {
        auto s = wide_to_utf8(fwVer);
        strncpy_s(out.firmware_version, sizeof(out.firmware_version), s.c_str(), _TRUNCATE);
    }

    if (!sn.empty())
    {
        auto s = wide_to_utf8(sn);
        strncpy_s(out.serial_number, sizeof(out.serial_number), s.c_str(), _TRUNCATE);
    }

    SetupDiDestroyDeviceInfoList(set);
    return true;
}

bool WinMFProvider::getSignalStatus(gcap_signal_status_t &out)
{
    memset(&out, 0, sizeof(out));
    out.width = cur_w_;
    out.height = cur_h_;
    out.fps_num = (cur_fps_num_ > 0) ? cur_fps_num_ : 0;
    out.fps_den = (cur_fps_den_ > 0) ? cur_fps_den_ : 1;
    out.pixfmt = mfsub_to_gcap(cur_subtype_);
    out.bit_depth = pixfmt_bitdepth(out.pixfmt);
    out.csp = GCAP_CSP_UNKNOWN;
    out.range = GCAP_RANGE_UNKNOWN;
    out.hdr = -1;
    return (cur_w_ > 0 && cur_h_ > 0);
}

bool WinMFProvider::setProcessing(const gcap_processing_opts_t &opts)
{
    (void)opts;
    // 先回不支援：等你要做「切 NV12/YUY2/P010 / Deinterlace」再補 setProfile / rebuild reader
    return false;
}

// ---- logging helpers (for negotiated media type / stride debug) ----
static const char *mf_subtype_name(const GUID &g)
{
    if (g == MFVideoFormat_NV12)
        return "NV12";
    if (g == MFVideoFormat_P010)
        return "P010";
    if (g == MFVideoFormat_YUY2)
        return "YUY2";
    if (g == MFVideoFormat_ARGB32)
        return "ARGB32";
    if (g == MFVideoFormat_RGB32)
        return "RGB32";
    if (g == MFVideoFormat_MJPG)
        return "MJPG";
    return "(unknown)";
}

static int mf_default_stride_bytes(IMFMediaType *mt)
{
    if (!mt)
        return 0;
    UINT32 raw = 0;
    if (FAILED(mt->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw)))
        return 0;
    // MF_MT_DEFAULT_STRIDE is effectively an INT32 stored in a UINT32 container.
    int s = (int)(int32_t)raw;
    return s < 0 ? -s : s;
}

// UTF-8 → UTF-16 (wstring) 工具，用來把檔名丟給 Media Foundation
static std::wstring utf8_to_wstring(const char *s)
{
    if (!s)
        return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 0)
        return std::wstring();
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &ws[0], len);
    return ws;
}

// ------------------------------------------------------------
// Step 3-2: WASAPI capture (default eCapture endpoint)
//  - Shared mode
//  - 48k/2ch/16-bit PCM (let audio engine do mix)
//  - Event-driven capture
// ------------------------------------------------------------
class WasapiCapture
{
public:
    struct Chunk
    {
        LONGLONG ts100ns = 0;     // relative timeline
        LONGLONG dur100ns = 0;    // duration
        std::vector<uint8_t> pcm; // interleaved bytes (PCM16 or Float32)
    };

    struct ActualFormat
    {
        UINT32 sampleRate = 0;
        UINT32 channels = 0;
        UINT32 bits = 0;
        bool isFloat = false;
        UINT32 blockAlign = 0;
    };

    // actual capture format from audio engine (may be float32)
    struct CaptureFormat
    {
        UINT32 sampleRate = 0;
        UINT32 channels = 0;
        UINT32 bits = 0;
        bool isFloat = false;
        UINT32 blockAlign = 0;
    };

    bool start(UINT32 sampleRate, UINT32 channels, UINT32 bits,
               const std::wstring &endpointId, ActualFormat *outFmt = nullptr)
    {
        stop();
        // Reset device-position timeline base for each start
        haveBasePos_ = false;
        basePos_ = 0;

        sampleRate_ = sampleRate;
        channels_ = channels;
        bits_ = bits;
        endpointId_ = endpointId;
        {
            std::lock_guard<std::mutex> lk(initMutex_);
            initDone_ = false;
            initOk_ = false;
            actual_ = {};
            cap_ = {};
        }

        running_.store(true);
        thread_ = std::thread([this]()
                              { this->run(); });
        // Wait for init result so caller can decide whether to enable audio

        std::unique_lock<std::mutex> ulk(initMutex_);
        initCv_.wait_for(ulk, std::chrono::milliseconds(800), [this]()
                         { return initDone_; });
        if (outFmt)
            *outFmt = actual_;
        return initOk_;
    }

    void stop()
    {
        running_.store(false);
        if (event_)
            SetEvent(event_);
        if (thread_.joinable())
            thread_.join();

        if (captureClient_)
            captureClient_.Reset();
        if (audioClient_)
            audioClient_.Reset();
        if (dev_)
            dev_.Reset();
        if (enumerator_)
            enumerator_.Reset();
        if (event_)
        {
            CloseHandle(event_);
            event_ = nullptr;
        }

        std::lock_guard<std::mutex> lk(mutex_);
        queue_.clear();
        tsCursor100ns_ = 0;
    }

    // non-blocking pop
    bool pop(Chunk &out)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (queue_.empty())
            return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    // wait until queue has data or stopped (does NOT consume)
    bool waitForData(int timeoutMs)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]()
                          { return !queue_.empty() || !running_.load(); }))
            return false;
        return !queue_.empty();
    }

private:
    void run()
    {
        // COM init for this thread
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto notifyInit = [&](bool ok)
        {
            std::lock_guard<std::mutex> lk(initMutex_);
            initDone_ = true;
            initOk_ = ok;
            actual_.sampleRate = sampleRate_;
            actual_.channels = channels_;
            actual_.bits = bits_;
            actual_.isFloat = isFloat_;
            actual_.blockAlign = blockAlign_;
            initCv_.notify_all();
        };

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator_));
        if (FAILED(hr))
        {
            notifyInit(false);
            CoUninitialize();
            return;
        }

        // Use selected endpoint if provided; otherwise fall back to default.
        if (!endpointId_.empty())
        {
            hr = enumerator_->GetDevice(endpointId_.c_str(), &dev_);
            if (FAILED(hr))
            {
                // Device may have been removed / id invalid → fallback to default.
                hr = enumerator_->GetDefaultAudioEndpoint(eCapture, eConsole, &dev_);
            }
        }
        else
        {
            hr = enumerator_->GetDefaultAudioEndpoint(eCapture, eConsole, &dev_);
        }

        if (FAILED(hr))
        {
            notifyInit(false);
            CoUninitialize();
            return;
        }

        hr = dev_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&audioClient_);
        if (FAILED(hr))
        {
            notifyInit(false);
            CoUninitialize();
            return;
        }

        // Request format (preferred), but fallback to mix format if unsupported.
        WAVEFORMATEX req{};
        req.wFormatTag = WAVE_FORMAT_PCM;
        req.nChannels = (WORD)channels_;
        req.nSamplesPerSec = sampleRate_;
        req.wBitsPerSample = (WORD)bits_;
        req.nBlockAlign = (req.nChannels * req.wBitsPerSample) / 8;
        req.nAvgBytesPerSec = req.nSamplesPerSec * req.nBlockAlign;

        // event-driven + allow engine conversion when possible
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        const REFERENCE_TIME bufferDur = 1'000'000; // 100ms (stable recording priority)

        // try requested
        hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDur, 0, &req, nullptr);
        if (FAILED(hr))
        {
            // fallback: mix format
            WAVEFORMATEX *mix = nullptr;
            if (SUCCEEDED(audioClient_->GetMixFormat(&mix)) && mix)
            {
                // parse mix format (engine capture format)
                cap_.blockAlign = mix->nBlockAlign;
                cap_.sampleRate = mix->nSamplesPerSec;
                cap_.channels = mix->nChannels;
                cap_.bits = mix->wBitsPerSample;
                cap_.isFloat = false;
                if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
                    cap_.isFloat = true;
                else if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                         mix->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)))
                {
                    auto ext = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix);
                    if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                        cap_.isFloat = true;
                }

                hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDur, 0, mix, nullptr);
                CoTaskMemFree(mix);
            }
            if (FAILED(hr))
            {
                notifyInit(false);
                CoUninitialize();
                return;
            }
        }

        // We ALWAYS output PCM16 to upper layer (match MF input media type),
        // but the engine capture format may be float32 if we used mix format.
        if (cap_.sampleRate == 0)
        {
            // requested worked => capture format == requested
            cap_.sampleRate = sampleRate_;
            cap_.channels = channels_;
            cap_.bits = bits_;
            cap_.isFloat = false;
            cap_.blockAlign = channels_ * (bits_ / 8);
        }

        // force output format = requested (typically PCM16)
        isFloat_ = false;
        if (blockAlign_ == 0)
            blockAlign_ = channels_ * (bits_ / 8);

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_)
        {
            notifyInit(false);
            CoUninitialize();
            return;
        }
        hr = audioClient_->SetEventHandle(event_);
        if (FAILED(hr))
        {
            notifyInit(false);
            CoUninitialize();
            return;
        }

        hr = audioClient_->GetService(IID_PPV_ARGS(&captureClient_));
        if (FAILED(hr))
        {
            notifyInit(false);
            CoUninitialize();
            return;
        }

        hr = audioClient_->Start();
        if (FAILED(hr))
        {
            notifyInit(false);
            CoUninitialize();
            return;
        }

        notifyInit(true);

        // capture loop
        while (running_.load())
        {
            // wait signal
            WaitForSingleObject(event_, 20);
            if (!running_.load())
                break;

            UINT32 packet = 0;
            hr = captureClient_->GetNextPacketSize(&packet);
            if (FAILED(hr))
                continue;

            while (packet > 0)
            {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags2 = 0;
                UINT64 devPos = 0; // in frames
                UINT64 qpcPos = 0; // QPC ticks
                hr = captureClient_->GetBuffer(&data, &frames, &flags2, &devPos, &qpcPos);
                if (FAILED(hr))
                    break;
                // Use device position to build an accurate timeline even if discontinuities happen.
                // devPos is the position (in frames) of the first frame in this packet since stream start.
                if (!haveBasePos_)
                {
                    basePos_ = devPos;
                    haveBasePos_ = true;
                }

                const UINT32 bytesPerFrame = (blockAlign_ != 0) ? blockAlign_ : (channels_ * (bits_ / 8));
                const UINT32 bytes = frames * bytesPerFrame;

                Chunk ck;
                // Timestamp derived from device position, not local cursor.
                const UINT64 relFrames = (devPos >= basePos_) ? (devPos - basePos_) : 0;
                ck.ts100ns = (LONGLONG)relFrames * 10'000'000LL / (LONGLONG)sampleRate_;
                ck.dur100ns = (LONGLONG)frames * 10'000'000LL / (LONGLONG)sampleRate_;
                const UINT32 outBytes = bytes;
                ck.pcm.resize(outBytes);

                if (flags2 & AUDCLNT_BUFFERFLAGS_SILENT || !data)
                {
                    memset(ck.pcm.data(), 0, outBytes);
                }
                else
                {
                    // Convert if engine gives float32 but we want PCM16
                    if (cap_.isFloat && cap_.bits == 32 && bits_ == 16)
                    {
                        // interleaved float32 [-1,1] -> int16
                        const float *src = reinterpret_cast<const float *>(data);
                        int16_t *dst16 = reinterpret_cast<int16_t *>(ck.pcm.data());
                        const size_t samples = (size_t)frames * (size_t)channels_;
                        for (size_t i = 0; i < samples; ++i)
                        {
                            float v = src[i];
                            if (v > 1.0f)
                                v = 1.0f;
                            if (v < -1.0f)
                                v = -1.0f;
                            dst16[i] = (int16_t)std::lroundf(v * 32767.0f);
                        }
                    }
                    else
                    {
                        // same format => direct copy (assume PCM16)
                        memcpy(ck.pcm.data(), data, outBytes);
                    }
                }

                captureClient_->ReleaseBuffer(frames);

                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    // cap queue length (~2 seconds for stable recording)
                    const size_t maxQueue = (size_t)200; // 10ms * 200 = 2s
                    if (queue_.size() > maxQueue)
                        queue_.pop_front();
                    queue_.push_back(std::move(ck));
                }
                cv_.notify_one();

                // Keep cursor for debug only; real timestamp uses devPos.
                tsCursor100ns_ = ck.ts100ns + ck.dur100ns;

                hr = captureClient_->GetNextPacketSize(&packet);
                if (FAILED(hr))
                    break;
            }
        }

        audioClient_->Stop();
        CoUninitialize();
    }

private:
    std::atomic<bool> running_{false};
    std::thread thread_;
    HANDLE event_ = nullptr;

    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> dev_;
    ComPtr<IAudioClient> audioClient_;
    ComPtr<IAudioCaptureClient> captureClient_;

    UINT32 sampleRate_ = 48000;
    UINT32 channels_ = 2;
    UINT32 bits_ = 16;
    std::wstring endpointId_;
    bool isFloat_ = false;
    UINT32 blockAlign_ = 0;
    CaptureFormat cap_{};
    // Device-position timeline base (reset each start)
    bool haveBasePos_ = false;
    UINT64 basePos_ = 0;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Chunk> queue_;
    LONGLONG tsCursor100ns_ = 0;
    std::mutex initMutex_;
    std::condition_variable initCv_;
    bool initDone_ = false;
    bool initOk_ = false;
    ActualFormat actual_{};
};

// ---- H.264 / HEVC 錄影器（Media Foundation Sink Writer, 以 NV12 / P010 為輸入）----
struct WinMFProvider::MfRecorder
{
    ComPtr<IMFSinkWriter> writer;
    INT32 fpsN = 0, fpsD = 1;
    DWORD streamIndex = 0;
    // ---- Audio (real WASAPI PCM -> AAC) ----
    DWORD audioStreamIndex = 0;
    std::thread audioThread;
    std::atomic<bool> audioRunning{false};
    std::mutex writerMutex; // protect SinkWriter from concurrent WriteSample
    bool hasAudio = false;
    UINT32 audioSampleRate = 48000;
    UINT32 audioChannels = 2;
    UINT32 audioBits = 16; // PCM 16-bit
    bool audioIsFloat = false;
    UINT32 audioBlockAlign = 0;

    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 fpsNum = 0;
    UINT32 fpsDen = 1;
    bool isP010 = false;        // false: NV12 → H.264, true: P010 → HEVC
    LONGLONG firstTs100ns = -1; // 第一幀時間當作 0

    WasapiCapture wasapi; // WASAPI capture + queue

    // audio timeline state (relative 100ns, 0-based)
    LONGLONG lastAudioTs100ns = 0;

    void stopAudioThread()
    {
        audioRunning.store(false);
        wasapi.stop();
        if (audioThread.joinable())
            audioThread.join();
    }

    void close()
    {
        stopAudioThread();
        if (writer)
        {
            HRESULT hr = writer->Finalize();
            if (FAILED(hr))
                OutputDebugStringA(("[WinMF][Rec] Finalize failed: " + hr_msg(hr) + "\n").c_str());
            writer.Reset();
        }
        firstTs100ns = -1;
        hasAudio = false;
        lastAudioTs100ns = 0;
    }

    bool writeAudioDrainOnce()
    {
        if (!writer || !hasAudio)
            return true;

        const UINT32 bytesPerSample = audioBits / 8;
        const UINT32 blockAlign = (audioBlockAlign != 0) ? audioBlockAlign : (audioChannels * bytesPerSample);

        // First: drain real WASAPI chunks as much as possible
        WasapiCapture::Chunk ck;
        while (wasapi.pop(ck))
        {
            // If gap exists, fill it with silence (gap-based, not video-based)
            if (ck.ts100ns > lastAudioTs100ns)
            {
                const LONGLONG gap100ns = ck.ts100ns - lastAudioTs100ns;
                const UINT32 framesGap = (UINT32)((gap100ns * (LONGLONG)audioSampleRate) / 10'000'000LL);
                // write silence in <=50ms pieces
                const UINT32 maxFramesPerSil = audioSampleRate / 20; // 50ms
                UINT32 remain = framesGap;
                while (remain > 0)
                {
                    const UINT32 f = std::min(remain, maxFramesPerSil);
                    const DWORD bytes = f * blockAlign;
                    ComPtr<IMFSample> s;
                    if (FAILED(MFCreateSample(&s)))
                        return false;
                    ComPtr<IMFMediaBuffer> b;
                    if (FAILED(MFCreateMemoryBuffer(bytes, &b)))
                        return false;
                    BYTE *dst = nullptr;
                    DWORD maxLen = 0;
                    if (FAILED(b->Lock(&dst, &maxLen, nullptr)))
                        return false;
                    memset(dst, 0, bytes);
                    b->Unlock();
                    b->SetCurrentLength(bytes);
                    if (FAILED(s->AddBuffer(b.Get())))
                        return false;
                    const LONGLONG dur = (LONGLONG)f * 10'000'000LL / (LONGLONG)audioSampleRate;
                    s->SetSampleTime(lastAudioTs100ns);
                    s->SetSampleDuration(dur);
                    {
                        std::lock_guard<std::mutex> lk(writerMutex);
                        if (FAILED(writer->WriteSample(audioStreamIndex, s.Get())))
                            return false;
                    }
                    lastAudioTs100ns += dur;
                    remain -= f;
                }
            }
            // If we already advanced the audio timeline past this chunk (e.g., due to silence fill),
            // writing it would create overlapping / out-of-order timestamps and audible glitches.
            if (ck.ts100ns + ck.dur100ns <= lastAudioTs100ns)
                continue;
            // if there is a gap between lastAudioTs100ns and ck.ts100ns, we'll fill later by silence
            // write real chunk at its timeline position
            ComPtr<IMFSample> sample;
            HRESULT hr = MFCreateSample(&sample);
            if (FAILED(hr))
                return false;

            ComPtr<IMFMediaBuffer> buf;
            hr = MFCreateMemoryBuffer((DWORD)ck.pcm.size(), &buf);
            if (FAILED(hr))
                return false;

            BYTE *dst = nullptr;
            DWORD maxLen = 0;
            hr = buf->Lock(&dst, &maxLen, nullptr);
            if (FAILED(hr))
                return false;
            memcpy(dst, ck.pcm.data(), ck.pcm.size());
            buf->Unlock();
            buf->SetCurrentLength((DWORD)ck.pcm.size());
            hr = sample->AddBuffer(buf.Get());
            if (FAILED(hr))
                return false;

            // If audio starts earlier than video timeline (we start from 0), it's fine.
            sample->SetSampleTime(ck.ts100ns);
            sample->SetSampleDuration(ck.dur100ns);

            std::lock_guard<std::mutex> lk(writerMutex);
            hr = writer->WriteSample(audioStreamIndex, sample.Get());

            if (FAILED(hr))
                return false;

            // keep cursor monotonic for silence fill
            if (ck.ts100ns + ck.dur100ns > lastAudioTs100ns)
                lastAudioTs100ns = ck.ts100ns + ck.dur100ns;
        }

        return true;
    }

    bool open(const std::wstring &path, UINT32 w, UINT32 h,
              UINT32 fpsN, UINT32 fpsD, bool p010,
              const std::wstring &audioEndpointIdW)
    {
        close();

        if (path.empty() || !w || !h || !fpsN || !fpsD)
            return false;

        isP010 = p010;
        width = w;
        height = h;
        fpsNum = fpsN;
        fpsDen = fpsD;
        this->fpsN = fpsN;
        this->fpsD = fpsD;

        ComPtr<IMFSinkWriter> wtr;
        ComPtr<IMFMediaType> outType;
        ComPtr<IMFMediaType> inType;

        HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &wtr);
        if (FAILED(hr))
            return false;

        hasAudio = false;
        lastAudioTs100ns = 0;

        // 輸出：H.264 或 HEVC
        GUID outSub = isP010 ? MFVideoFormat_HEVC : MFVideoFormat_H264;

        hr = MFCreateMediaType(&outType);
        if (FAILED(hr))
            return false;

        hr = outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (FAILED(hr))
            return false;

        hr = outType->SetGUID(MF_MT_SUBTYPE, outSub);
        if (FAILED(hr))
            return false;

        // 簡單給一個 8Mbps 的 bitrate，之後可以再調整或改成參數
        hr = outType->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
        if (FAILED(hr))
            return false;

        hr = outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(hr))
            return false;

        hr = MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, w, h);
        if (FAILED(hr))
            return false;

        hr = MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, fpsN, fpsD);
        if (FAILED(hr))
            return false;

        hr = MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(hr))
            return false;

        DWORD idx = 0;
        hr = wtr->AddStream(outType.Get(), &idx);
        if (FAILED(hr))
            return false;

        // 輸入：NV12 或 P010
        GUID inSub = isP010 ? MFVideoFormat_P010 : MFVideoFormat_NV12;

        hr = MFCreateMediaType(&inType);
        if (FAILED(hr))
            return false;

        hr = inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (FAILED(hr))
            return false;

        hr = inType->SetGUID(MF_MT_SUBTYPE, inSub);
        if (FAILED(hr))
            return false;

        hr = MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, w, h);
        if (FAILED(hr))
            return false;

        hr = MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, fpsN, fpsD);
        if (FAILED(hr))
            return false;

        hr = MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(hr))
            return false;

        hr = wtr->SetInputMediaType(idx, inType.Get(), nullptr);
        if (FAILED(hr))
            return false;

        // ------------------------------
        // Add audio track (AAC out, PCM in) + start WASAPI capture
        // ------------------------------
        {
            ComPtr<IMFMediaType> outAud;
            ComPtr<IMFMediaType> inAud;

            // Output AAC (encoded)
            hr = MFCreateMediaType(&outAud);
            if (FAILED(hr))
                return false;

            hr = outAud->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            if (FAILED(hr))
                return false;

            hr = outAud->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            if (FAILED(hr))
                return false;

            hr = outAud->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audioChannels);
            if (FAILED(hr))
                return false;

            hr = outAud->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audioSampleRate);
            if (FAILED(hr))
                return false;

            // 128 kbps AAC (common default)
            const UINT32 aacBitrate = 128000;
            hr = outAud->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, aacBitrate / 8);
            if (FAILED(hr))
                return false;

            // These help some MFTs, harmless if present
            outAud->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, audioBits);

#ifdef MF_MT_AAC_PAYLOAD_TYPE
            outAud->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
#endif
#ifdef MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION
            // 0x29 is a commonly used value (AAC LC)
            outAud->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
#endif

            DWORD aidx = 0;
            hr = wtr->AddStream(outAud.Get(), &aidx);
            if (FAILED(hr))
                return false;

            // Input PCM (what we will feed; for Step 3-1 it's silence)
            hr = MFCreateMediaType(&inAud);
            if (FAILED(hr))
                return false;

            hr = inAud->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            if (FAILED(hr))
                return false;

            hr = inAud->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            if (FAILED(hr))
                return false;

            hr = inAud->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audioChannels);
            if (FAILED(hr))
                return false;

            hr = inAud->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audioSampleRate);
            if (FAILED(hr))
                return false;

            hr = inAud->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, audioBits);
            if (FAILED(hr))
                return false;

            const UINT32 blockAlign = audioChannels * (audioBits / 8);
            const UINT32 avgBytesSec = audioSampleRate * blockAlign;
            hr = inAud->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
            if (FAILED(hr))
                return false;
            hr = inAud->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytesSec);
            if (FAILED(hr))
                return false;

            hr = wtr->SetInputMediaType(aidx, inAud.Get(), nullptr);
            if (FAILED(hr))
                return false;

            audioStreamIndex = aidx;
            hasAudio = true;
        }

        hr = wtr->BeginWriting();
        if (FAILED(hr))
            return false;

        writer = wtr;
        streamIndex = idx;
        firstTs100ns = -1;
        lastAudioTs100ns = 0;

        // Start WASAPI capture (selected endpoint or default) and discover actual format if needed
        WasapiCapture::ActualFormat af{};
        if (wasapi.start(audioSampleRate, audioChannels, audioBits, audioEndpointIdW, &af))
        {
            hasAudio = true;
            audioSampleRate = af.sampleRate ? af.sampleRate : audioSampleRate;
            audioChannels = af.channels ? af.channels : audioChannels;
            audioBits = af.bits ? af.bits : audioBits;
            audioIsFloat = af.isFloat;
            audioBlockAlign = af.blockAlign;
        }
        else
        {
            // keep recording video even if audio failed
            hasAudio = false;
        }

        // If audio enabled, start independent audio writer thread (stable recording priority)
        if (hasAudio)
        {
            audioRunning.store(true);
            audioThread = std::thread([this]()
                                      {
                while (audioRunning.load())
                {
                    // wait for audio data or timeout; drain whatever we have
                    wasapi.waitForData(50);
                    if (!writeAudioDrainOnce())
                        break;
                }
                // final drain
                writeAudioDrainOnce(); });
        }

        // ---- Log 實際使用的 Sink Writer 設定 ----
        {
            const char *codecName = isP010 ? "HEVC/H.265" : "H.264/AVC";
            const char *inputName = isP010 ? "P010 10-bit" : "NV12 8-bit";
            const UINT32 kbps = 8000000 / 1000;

            std::ostringstream oss;
            oss << "[WinMF] Recorder open: codec=" << codecName
                << ", input=" << inputName
                << ", " << w << "x" << h
                << " @ " << fpsN;
            if (fpsD != 1)
                oss << "/" << fpsD;
            oss << " fps"
                << ", target bitrate=" << kbps << " kbps\n";

            OutputDebugStringA(oss.str().c_str());
        }

        return true;
    }

    bool writePlanar(const uint8_t *y, const uint8_t *uv,
                     UINT32 yStrideBytes, UINT32 uvStrideBytes,
                     LONGLONG ts100ns)
    {
        if (!writer || !y || !uv)
            return false;

        if (firstTs100ns < 0)
            firstTs100ns = ts100ns;

        const UINT32 w = width;
        const UINT32 h = height;

        // tight packed stride (no padding)
        const UINT32 bpp = isP010 ? 2 : 1; // NV12:1 byte, P010:2 bytes per sample
        const UINT32 rowBytesY_tight = w * bpp;
        const UINT32 rowBytesUV_tight = w * bpp;

        const DWORD yBytes = rowBytesY_tight * h;
        const DWORD uvBytes = rowBytesUV_tight * (h / 2);
        const DWORD frameBytes = yBytes + uvBytes;

        ComPtr<IMFSample> sample;
        HRESULT hr = MFCreateSample(&sample);
        if (FAILED(hr))
            return false;

        ComPtr<IMFMediaBuffer> buf;
        hr = MFCreateMemoryBuffer(frameBytes, &buf);
        if (FAILED(hr))
            return false;

        BYTE *dst = nullptr;
        DWORD maxLen = 0;
        hr = buf->Lock(&dst, &maxLen, nullptr);
        if (FAILED(hr) || !dst || maxLen < frameBytes)
            return false;

        BYTE *dstY = dst;
        BYTE *dstUV = dst + yBytes;

        // copy Y (only valid width, ignore source padding)
        for (UINT32 row = 0; row < h; ++row)
        {
            memcpy(dstY + rowBytesY_tight * row,
                   y + yStrideBytes * row,
                   rowBytesY_tight);
        }

        // copy UV (h/2 rows)
        for (UINT32 row = 0; row < h / 2; ++row)
        {
            memcpy(dstUV + rowBytesUV_tight * row,
                   uv + uvStrideBytes * row,
                   rowBytesUV_tight);
        }

        buf->Unlock();
        buf->SetCurrentLength(frameBytes);

        hr = sample->AddBuffer(buf.Get());
        if (FAILED(hr))
            return false;

        // timestamp (relative)
        LONGLONG rtStart = ts100ns - firstTs100ns;
        sample->SetSampleTime(rtStart);

        // duration: 1 frame
        // vFpsN/vFpsD are already stored in recorder (if you don't have them, compute from profile)
        if (fpsN != 0)
        {
            const LONGLONG vDuration = (10'000'000LL * (LONGLONG)fpsD) / (LONGLONG)fpsN;
            sample->SetSampleDuration(vDuration);
        }

        std::lock_guard<std::mutex> lk(writerMutex);
        hr = writer->WriteSample(streamIndex, sample.Get());
        if (FAILED(hr))
        {
            OutputDebugStringA(("[WinMF][Rec] video WriteSample failed: " + hr_msg(hr) + "\n").c_str());
            return false;
        }

        return true;
    }

    // 8-bit NV12 → H.264
    bool writeNV12(const uint8_t *y, const uint8_t *uv,
                   UINT32 yStride, UINT32 uvStride,
                   LONGLONG ts100ns)
    {
        // 這個 recorder 是 P010 模式的話就不要誤用
        if (isP010)
            return false;

        return writePlanar(y, uv, yStride, uvStride, ts100ns);
    }

    // 10-bit P010 → HEVC
    bool writeP010(const uint8_t *y, const uint8_t *uv,
                   UINT32 yStrideBytes, UINT32 uvStrideBytes,
                   LONGLONG ts100ns)
    {
        // 這個 recorder 不是 P010 模式就不要誤用
        if (!isP010)
            return false;

        return writePlanar(y, uv, yStrideBytes, uvStrideBytes, ts100ns);
    }
};

gcap_status_t WinMFProvider::startRecording(const char *pathUtf8)
{
    std::lock_guard<std::mutex> lock(recorderMutex_);

    if (!reader_) // 尚未 open / start
        return GCAP_ESTATE;

    if (!pathUtf8 || !*pathUtf8)
        return GCAP_EINVAL;

    // 目前只支援 NV12 / P010 兩種 YUV 型態
    bool isP010Format = false;
    if (cur_subtype_ == MFVideoFormat_P010)
    {
        isP010Format = true;
    }
    else if (cur_subtype_ == MFVideoFormat_NV12)
    {
        isP010Format = false;
    }
    else
    {
        return GCAP_ENOTSUP;
    }

    if (!recorder_)
        recorder_ = std::make_unique<MfRecorder>();

    UINT32 w = static_cast<UINT32>(cur_w_);
    UINT32 h = static_cast<UINT32>(cur_h_);
    UINT32 fpsN = profile_.fps_num ? profile_.fps_num : 60;
    UINT32 fpsD = profile_.fps_den ? profile_.fps_den : 1;

    std::wstring wpath = utf8_to_wstring(pathUtf8);
    // recording audio endpoint (empty => default)
    std::wstring audioIdW;
    if (!rec_audio_device_id_.empty())
        audioIdW = utf8_to_wstring(rec_audio_device_id_.c_str());

    if (!recorder_->open(wpath, w, h, fpsN, fpsD, isP010Format, audioIdW))
        return GCAP_EIO;

    OutputDebugStringA("[WinMF] Recorder: startRecording()\\n");
    return GCAP_OK;
}

gcap_status_t WinMFProvider::stopRecording()
{
    std::lock_guard<std::mutex> lock(recorderMutex_);

    if (recorder_)
    {
        recorder_->close();
        OutputDebugStringA("[WinMF] Recorder: stopRecording()\\n");
    }
    return GCAP_OK;
}

gcap_status_t WinMFProvider::setRecordingAudioDevice(const char *device_id_utf8)
{
    std::lock_guard<std::mutex> lock(recorderMutex_);

    // Only affects next startRecording call.
    rec_audio_device_id_.clear();
    if (device_id_utf8 && *device_id_utf8)
        rec_audio_device_id_ = device_id_utf8;

    OutputDebugStringA("[WinMF] Recorder audio endpoint set\n");
    return GCAP_OK;
}

#define DBG(stage, hr)                                                          \
    do                                                                          \
    {                                                                           \
        std::string __m = std::string("[WinMF] ") + stage + " : " + hr_msg(hr); \
        OutputDebugStringA((__m + "\n").c_str());                               \
    } while (0)

// Member-scope debug: also forward to app log (via emit_error(GCAP_OK,...)).
// IMPORTANT: Only use inside WinMFProvider member functions (where `this` exists).
#define MDBG(stage, hr)                                                         \
    do                                                                          \
    {                                                                           \
        std::string __m = std::string("[WinMF] ") + stage + " : " + hr_msg(hr); \
        this->emit_error(GCAP_OK, __m.c_str());                                 \
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

// 初始預設使用系統 default adapter（-1）
std::atomic<int> WinMFProvider::s_adapter_index_{-1};

void WinMFProvider::setPreferredAdapterIndex(int index)
{
    s_adapter_index_.store(index, std::memory_order_relaxed);
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
    {
        ecb_(c, msg, user_);
        return;
    }

    // callbacks 尚未設定：只暫存 GCAP_OK 的 debug 訊息（避免把錯誤吞掉）
    if (c == GCAP_OK && msg)
        pending_log_push(msg);
}

void WinMFProvider::pending_log_push(const char *msg)
{
    std::lock_guard<std::mutex> lk(pending_mtx_);
    if (pending_logs_.size() >= kMaxPendingLogs)
        pending_logs_.pop_front();
    pending_logs_.emplace_back(msg);
}

void WinMFProvider::pending_log_flush()
{
    // 把暫存 log 取出（避免在 lock 內呼叫 callback）
    std::deque<std::string> tmp;
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        tmp.swap(pending_logs_);
    }

    if (!ecb_)
        return;

    for (auto &s : tmp)
        ecb_(GCAP_OK, s.c_str(), user_);
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
        gcap_device_info_t di{};
        di.index = (int)i;
        di.caps = 0;

        // Friendly name
        std::wstring wname = get_mf_string(pp[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        if (!wname.empty())
        {
            std::string s = utf8_from_wide(wname.c_str());
            strncpy(di.name, s.c_str(), sizeof(di.name) - 1);
        }

        // Symbolic link（給後續查 Driver/FW/Serial）
        std::wstring wlink = get_mf_string(pp[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        if (!wlink.empty())
        {
            std::string s = utf8_from_wide(wlink.c_str());
            strncpy(di.symbolic_link, s.c_str(), sizeof(di.symbolic_link) - 1);
        }

        list.push_back(di);
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
    {
        std::wstring wname = get_mf_string(pp[devIndex], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        if (!wname.empty())
            dev_name_ = utf8_from_wide(wname.c_str());
        dev_sym_link_w_ = get_mf_string(pp[devIndex], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
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
            // 先嘗試強制協商成 NV12（協商不到就維持 device default，可能是 YUY2）
            // OBS-style: 不選最大 native format，直接使用裝置 default
            ComPtr<IMFMediaType> cur;
            if (SUCCEEDED(reader_->GetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)))
            {
                ComPtr<IMFMediaType> req;
                if (SUCCEEDED(MFCreateMediaType(&req)))
                {
                    req->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                    req->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                    HRESULT hrSet = reader_->SetCurrentMediaType(
                        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, req.Get());
                    if (SUCCEEDED(hrSet))
                    {
                        // 重新讀回實際 negotiated（通常會變 NV12）
                        reader_->GetCurrentMediaType(
                            MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
                    }
                }

                UINT32 w = 0, h = 0, fn = 0, fd = 1;
                MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
                MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &fn, &fd);
                cur->GetGUID(MF_MT_SUBTYPE, &cur_subtype_);

                cur_w_ = (int)w;
                cur_h_ = (int)h;
                cpu_path_ = false;

                ensure_rt_and_pipeline(cur_w_, cur_h_);

                emit_error(GCAP_OK,
                           "[WinMF] Using device default format (OBS-style)");
                return true;
            }
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

    cur->GetGUID(MF_MT_SUBTYPE, &cur_subtype_);

    // negotiated stride (very important for capture cards with aligned rows)
    cur_stride_ = mf_default_stride_bytes(cur.Get());
    if (cur_stride_ <= 0)
    {
        if (cur_subtype_ == MFVideoFormat_P010)
            cur_stride_ = cur_w_ * 2;
        else if (cur_subtype_ == MFVideoFormat_ARGB32)
            cur_stride_ = cur_w_ * 4;
        else
            cur_stride_ = cur_w_;
    }

    // negotiated media type (CPU/VP path)
    {
        std::ostringstream oss;
        oss << "[WinMF] negotiated (CPU/VP): dev='" << dev_name_ << "'"
            << ", subtype=" << mf_subtype_name(cur_subtype_)
            << ", " << cur_w_ << "x" << cur_h_
            << " @ " << fn;
        if (fd != 1)
            oss << "/" << fd;
        oss << " fps"
            << ", default_stride=" << cur_stride_ << " bytes";
        emit_error(GCAP_OK, oss.str().c_str());
    }

    // 只開第一個視訊串流
    reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    OutputDebugStringA("[WinMF] open(): using CPU pipeline\n");
    emit_error(GCAP_OK, "[WinMF] open(): using CPU pipeline");
    return true;
}

bool WinMFProvider::setProfile(const gcap_profile_t &p)
{
    profile_ = p;

    // OBS-style: Device Default 不強制設定解析度
    if (p.mode == GCAP_PROFILE_DEVICE_DEFAULT)
        return true;

    if (!reader_)
        return true;

    ComPtr<IMFMediaType> mt;
    if (FAILED(MFCreateMediaType(&mt)))
        return false;

    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, p.width, p.height);
    MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE,
                        p.fps_num ? p.fps_num : 60,
                        p.fps_den ? p.fps_den : 1);
    MFSetAttributeRatio(mt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    HRESULT hr = reader_->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt.Get());

    if (FAILED(hr))
    {
        emit_error(GCAP_OK,
                   "[WinMF] Custom profile rejected, fallback to device default");
        return false;
    }

    emit_error(GCAP_OK, "[WinMF] Custom profile applied");

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
    stopRecording();
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
    ps_yuy2_.Reset();
    cs_nv12_.Reset();
    cs_params_.Reset();
    rt_uav_.Reset();
    upload_yuy2_packed_.Reset();

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
    // callbacks 設定完成後，把 open() 階段的 pending logs 一次吐出
    pending_log_flush();
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

    // 目前選擇的 Adapter index（由 UI 經由 C API 設定）
    const int wantAdapter = s_adapter_index_.load(std::memory_order_relaxed);

    auto try_create = [&](IDXGIAdapter1 *adapter, UINT fl) -> HRESULT
    {
        return D3D11CreateDevice(adapter,
                                 adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr,
                                 fl,
                                 fls,
                                 _countof(fls),
                                 D3D11_SDK_VERSION,
                                 &d3d_,
                                 &got,
                                 &ctx_);
    };

    HRESULT hr = E_FAIL;

    // 1) 如果有指定 Adapter index，先嘗試用該張 GPU 建 device
    if (wantAdapter >= 0)
    {
        ComPtr<IDXGIFactory1> fac;
        hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(fac.GetAddressOf()));
        if (SUCCEEDED(hr) && fac)
        {
            ComPtr<IDXGIAdapter1> ad;
            hr = fac->EnumAdapters1(static_cast<UINT>(wantAdapter), &ad);
            if (SUCCEEDED(hr) && ad)
            {
                hr = try_create(ad.Get(), flags);
#ifdef _DEBUG
                if (FAILED(hr))
                {
                    // 失敗時移除 DEBUG 再試一次
                    flags &= ~D3D11_CREATE_DEVICE_DEBUG;
                    hr = try_create(ad.Get(), flags);
                }
#endif
                if (FAILED(hr))
                {
                    DBG("D3D11CreateDevice(adapter) failed, fallback to default", hr);
                }
            }
            else
            {
                DBG("EnumAdapters1(wantAdapter) failed, fallback to default", hr);
            }
        }
    }

    // 2) 如果沒有指定，或指定失敗 → 回到原本的 default adapter 流程
    if (!d3d_)
    {
        hr = try_create(nullptr, flags);
#ifdef _DEBUG
        if (FAILED(hr))
        {
            // 自動移除 DEBUG 再試一次
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = try_create(nullptr, flags);
        }
#endif
        if (FAILED(hr))
        {
            DBG("D3D11CreateDevice(default)", hr);
            return false;
        }
    }

    d3d_.As(&d3d1_);
    ctx_.As(&ctx1_);
    d3d_.As(&d3d1_);
    ctx_.As(&ctx1_);

    // 取得實際使用的 GPU 名稱，之後在 NV12→RGBA overlay 顯示
    gpu_name_w_.clear();
    {
        ComPtr<IDXGIDevice> dxDev;
        if (SUCCEEDED(d3d_.As(&dxDev)) && dxDev)
        {
            ComPtr<IDXGIAdapter> ad;
            if (SUCCEEDED(dxDev->GetAdapter(&ad)) && ad)
            {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(ad->GetDesc(&desc)))
                {
                    gpu_name_w_ = desc.Description; // wchar_t[128]
                }
            }
        }
    }

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
        // 存 FriendlyName + SymbolicLink（GPU path 也要）
        {
            std::wstring wname = get_mf_string(pp[devIndex], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
            if (!wname.empty())
                dev_name_ = utf8_from_wide(wname.c_str());
            dev_sym_link_w_ = get_mf_string(pp[devIndex], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        }

        for (UINT32 i = 0; i < count; ++i)
            pp[i]->Release();
        CoTaskMemFree(pp);
        return hr;
    };

    HRESULT hr = S_OK;

    MDBG("DXGI: Try DXGI+VP - begin", S_OK);
    hr = activate_source(source_);
    if (FAILED(hr))
    {
        MDBG("DXGI: activate_source failed", hr);
        return false;
    }

    ComPtr<IMFAttributes> rdAttr;
    hr = MFCreateAttributes(&rdAttr, 3);
    if (FAILED(hr))
    {
        MDBG("DXGI: MFCreateAttributes(reader) failed", hr);
        return false;
    }

    rdAttr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgi_mgr_.Get());
    rdAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    rdAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    if (!source_)
    {
        MDBG("DXGI: source_ is null before MFCreateSourceReaderFromMediaSource", 0);
        return false;
    }
    if (!dxgi_mgr_)
    {
        MDBG("DXGI: dxgi_mgr_ is null before MFCreateSourceReaderFromMediaSource", 0);
        return false;
    }

    hr = MFCreateSourceReaderFromMediaSource(source_.Get(), rdAttr.Get(), reader_.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        MDBG("DXGI: MFCreateSourceReaderFromMediaSource (with attr) failed", hr);

        // 如果是你現在遇到的 E_INVALIDARG，就改用「重新 activate 一顆新的 source 再建乾淨 reader」的方式重試
        if (hr == E_INVALIDARG)
        {
            MDBG("DXGI: retry MFCreateSourceReaderFromMediaSource with nullptr attr", hr);

            // 先把舊的 reader / source 都清掉，因為舊的 source 很可能已經被 Shutdown
            reader_.Reset();
            source_.Reset();

            // ★ 重新 activate 一顆新的 MediaSource（用同一個 devIndex）
            HRESULT hr2 = activate_source(source_);
            if (FAILED(hr2) || !source_)
            {
                MDBG("DXGI: activate_source (retry) failed", hr2);
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
                MDBG("DXGI: MFCreateSourceReaderFromMediaSource (no attr) also failed", hr);
                reader_.Reset();
                source_.Reset();
                return false;
            }

            MDBG("DXGI: CreateReader(no attr) succeeded, continue DXGI path", hr);
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
    MDBG("DXGI: DXGI+VP SUCCESS", hr);
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

        // Read back actual negotiated type (incl. stride)
        ComPtr<IMFMediaType> cur;
        if (SUCCEEDED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) && cur)
        {
            UINT32 rw = 0, rh = 0, rfn = 0, rfd = 1;
            GUID rsub = GUID_NULL;
            MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &rw, &rh);
            MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &rfn, &rfd);
            cur->GetGUID(MF_MT_SUBTYPE, &rsub);
            cur_fps_num_ = (int)rfn;
            cur_fps_den_ = (int)rfd;
            cur_stride_ = mf_default_stride_bytes(cur.Get());
            if (cur_stride_ <= 0)
            {
                if (rsub == MFVideoFormat_P010)
                    cur_stride_ = (int)rw * 2;
                else
                    cur_stride_ = (int)rw;
            }

            std::ostringstream oss;
            oss << "[WinMF] pick_best_native: subtype=" << mf_subtype_name(rsub)
                << ", " << rw << "x" << rh
                << " @ " << rfn;
            if (rfd != 1)
                oss << "/" << rfd;
            oss << " fps"
                << ", default_stride=" << cur_stride_ << " bytes";
            emit_error(GCAP_OK, oss.str().c_str());
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

// YUY2（4:2:2 packed）：
// 我們把每兩個像素打包成一個 RGBA8_UINT texel：
//   R=Y0, G=U, B=Y1, A=V
// texture width = ceil(w/2)
static const char *g_ps_yuy2 = R"(
Texture2D<uint4> texP : register(t0);

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
    int2 ip = int2(pos.xy);
    int px = ip.x;
    int py = ip.y;

    // 每兩個像素共用一組 U/V
    uint4 p = texP.Load(int3(px >> 1, py, 0)); // 0..255
    float y = ((px & 1) != 0 ? p.b : p.r) / 255.0;
    float u = (p.g / 255.0);
    float v = (p.a / 255.0);

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
    ComPtr<ID3DBlob> vsb, psb1, psb2, psb3, err;
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

    if (FAILED(D3DCompile(g_ps_yuy2, strlen(g_ps_yuy2), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psb3, &err)))
        return false;
    if (FAILED(d3d_->CreatePixelShader(psb3->GetBufferPointer(), psb3->GetBufferSize(), nullptr, &ps_yuy2_)))
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
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvP;
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
    else if (cur_subtype_ == MFVideoFormat_YUY2)
    {
        // YUY2：yuvTex 會是 upload_yuy2_packed_（RGBA8_UINT）
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(d3d_->CreateShaderResourceView(yuvTex, &sd, &srvP)))
            return false;
    }
    else
    {
        return false;
    }

    if (cur_subtype_ == MFVideoFormat_YUY2)
    {
        if (!srvP)
            return false;
    }
    else
    {
        if (!srvY || !srvUV)
            return false;
    }

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

    ID3D11PixelShader *ps =
        (cur_subtype_ == MFVideoFormat_NV12)   ? ps_nv12_.Get()
        : (cur_subtype_ == MFVideoFormat_P010) ? ps_p010_.Get()
                                               : ps_yuy2_.Get();
    ctx_->PSSetShader(ps, nullptr, 0);

    if (cur_subtype_ == MFVideoFormat_YUY2)
    {
        ID3D11ShaderResourceView *srvs0[1] = {srvP.Get()};
        ctx_->PSSetShaderResources(0, 1, srvs0);
        // 清掉 t1（避免殘留）
        ID3D11ShaderResourceView *null1[1] = {nullptr};
        ctx_->PSSetShaderResources(1, 1, null1);
    }
    else
    {
        ID3D11ShaderResourceView *srvs[2] = {srvY.Get(), srvUV.Get()};
        ctx_->PSSetShaderResources(0, 2, srvs);
    }

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
    if (cur_subtype_ == MFVideoFormat_YUY2)
    {
        ID3D11ShaderResourceView *null0[1] = {nullptr};
        ctx_->PSSetShaderResources(0, 1, null0);
    }
    else
    {
        ID3D11ShaderResourceView *nulls[2] = {nullptr, nullptr};
        ctx_->PSSetShaderResources(0, 2, nulls);
    }

    return true;
}

bool WinMFProvider::gpu_overlay_text(const wchar_t *text)
{
    if (!text || !*text || !d2d_ctx_ || !dwrite_)
        return true;

    d2d_ctx_->BeginDraw();
    d2d_ctx_->SetTransform(D2D1::Matrix3x2F::Identity());

    // ---------- 建立 TextFormat ----------
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = dwrite_->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, // 比較醒目
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

    // ---------- 用 TextLayout 量測文字寬高 ----------
    float layoutWidth = static_cast<float>(cur_w_) - 32.0f;
    if (layoutWidth < 100.0f)
        layoutWidth = 100.0f;
    float layoutHeight = 100.0f; // 單行已足夠

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    hr = dwrite_->CreateTextLayout(
        text,
        (UINT32)wcslen(text),
        fmt.Get(),
        layoutWidth,
        layoutHeight,
        &layout);
    if (FAILED(hr))
    {
        d2d_ctx_->EndDraw();
        return false;
    }

    DWRITE_TEXT_METRICS metrics{};
    hr = layout->GetMetrics(&metrics);
    if (FAILED(hr))
    {
        d2d_ctx_->EndDraw();
        return false;
    }

    float textWidth = metrics.width;
    float textHeight = metrics.height;

    // ---------- 黑底 + padding ----------
    const float padX = 12.0f;
    const float padY = 6.0f;

    D2D1_RECT_F bg = D2D1::RectF(
        8.0f,
        8.0f,
        8.0f + textWidth + padX * 2.0f,
        8.0f + textHeight + padY * 2.0f);

    // 半透明黑底
    d2d_ctx_->FillRectangle(bg, d2d_black_.Get());

    // ---------- 畫文字 ----------
    D2D1_RECT_F rc = D2D1::RectF(
        bg.left + padX,
        bg.top + padY,
        bg.right - padX,
        bg.bottom - padY);

    d2d_ctx_->DrawText(
        text,
        (UINT32)wcslen(text),
        fmt.Get(),
        rc,
        d2d_white_.Get());

    hr = d2d_ctx_->EndDraw();
    return SUCCEEDED(hr);
}

// -------------------- Capture loop --------------------

void WinMFProvider::loop()
{
    // Log stride/buffer length diagnostics only once per run (avoid spamming).
    bool logged_layout = false;
    bool logged_len_mismatch = false;

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

            // (1) log negotiated stride vs code assumption
            if (!logged_layout)
            {
                std::ostringstream oss;
                oss << "[WinMF] buffer layout (CPU): subtype=" << mf_subtype_name(cur_subtype_)
                    << ", curLen=" << curLen << ", maxLen=" << maxLen
                    << ", negotiated_stride=" << cur_stride_ << " bytes"
                    << ", code_assumes_stride=";
                if (cur_subtype_ == MFVideoFormat_P010)
                    oss << (cur_w_ * 2);
                else if (cur_subtype_ == MFVideoFormat_ARGB32)
                    oss << (cur_w_ * 4);
                else
                    oss << cur_w_;
                oss << " bytes";
                emit_error(GCAP_OK, oss.str().c_str());
                logged_layout = true;
            }

            // (2) bufferLen vs expectedLen
            if (!logged_len_mismatch)
            {
                const int stride = (cur_stride_ > 0) ? cur_stride_ : (cur_subtype_ == MFVideoFormat_P010) ? (cur_w_ * 2)
                                                                 : (cur_subtype_ == MFVideoFormat_ARGB32) ? (cur_w_ * 4)
                                                                                                          : cur_w_;
                size_t expected = 0;
                if (cur_subtype_ == MFVideoFormat_NV12)
                    expected = (size_t)stride * (size_t)cur_h_ + (size_t)stride * (size_t)(cur_h_ / 2);
                else if (cur_subtype_ == MFVideoFormat_P010)
                    expected = (size_t)stride * (size_t)cur_h_ + (size_t)stride * (size_t)(cur_h_ / 2);
                else if (cur_subtype_ == MFVideoFormat_ARGB32)
                    expected = (size_t)stride * (size_t)cur_h_;

                if (expected != 0 && (size_t)curLen < expected)
                {
                    std::ostringstream oss;
                    oss << "[WinMF] WARNING: bufferLen < expected (CPU): curLen=" << curLen
                        << ", expected>=" << expected
                        << ", subtype=" << mf_subtype_name(cur_subtype_)
                        << ", w=" << cur_w_ << ", h=" << cur_h_
                        << ", default_stride=" << stride;
                    emit_error(GCAP_OK, oss.str().c_str());
                    logged_len_mismatch = true;
                }
            }

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
                const int yStride = (cur_stride_ > 0) ? cur_stride_ : cur_w_;
                const int uvStride = yStride;
                const uint8_t *y = pData;
                const uint8_t *uv = pData + yStride * cur_h_;

                // --- Recording: NV12 直接送進 Sink Writer (H.264) ---
                {
                    std::lock_guard<std::mutex> lock(recorderMutex_);
                    if (recorder_)
                    {
                        recorder_->writeNV12(y, uv,
                                             static_cast<UINT32>(yStride),
                                             static_cast<UINT32>(uvStride),
                                             ts);
                    }
                }

                const size_t needed = (size_t)cur_w_ * (size_t)cur_h_ * 4;
                if (cpu_argb_.size() < needed)
                    cpu_argb_.resize(needed);

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
                const int yuy2Stride = (cur_stride_ > 0) ? cur_stride_ : (cur_w_ * 2);
                const uint8_t *yuy2 = pData;

                const size_t needed = (size_t)cur_w_ * (size_t)cur_h_ * 4;
                if (cpu_argb_.size() < needed)
                    cpu_argb_.resize(needed);

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
                MDBG("DXGI: GetResource from IMFDXGIBuffer failed", E_FAIL);
                continue;
            }
            dxgibuf->GetSubresourceIndex(&subres);
        }
        else
        {
            // ★ 沒有 IMFDXGIBuffer：從 CPU NV12/P010 buffer Upload 到 D3D11 texture，再走 shader
            BYTE *pData = nullptr;
            DWORD maxLen = 0, curLen = 0;

            // 作法 A：優先走 IMF2DBuffer，拿到「真正的 pitch」
            ComPtr<IMF2DBuffer> buf2d;
            LONG srcPitchLong = 0;
            bool locked2d = false;

            HRESULT hr2d = buf.As(&buf2d);
            if (SUCCEEDED(hr2d) && buf2d)
            {
                hr2d = buf2d->Lock2D(&pData, &srcPitchLong);
                if (SUCCEEDED(hr2d) && pData)
                {
                    locked2d = true;
                    // Lock2D 可能回傳負 pitch（top-down/bottom-up），這裡統一用絕對值當 stride
                    if (srcPitchLong < 0)
                        srcPitchLong = -srcPitchLong;
                    // curLen/maxLen 對 2D buffer 不一定可靠；保留 0 也沒關係（後面不再依賴它做 offset）
                }
            }

            if (!locked2d)
            {
                if (FAILED(buf->Lock(&pData, &maxLen, &curLen)) || !pData)
                    continue;
            }

            // (GPU upload fallback) log stride/bufferLen once
            if (!logged_layout)
            {
                std::ostringstream oss;
                oss << "[WinMF] buffer layout (GPU-upload fallback): subtype=" << mf_subtype_name(cur_subtype_)
                    << ", curLen=" << curLen << ", maxLen=" << maxLen
                    << ", negotiated_stride=" << cur_stride_ << " bytes"
                    << ", code_assumes_stride=";
                if (cur_subtype_ == MFVideoFormat_P010)
                    oss << (cur_w_ * 2);
                else
                    oss << cur_w_;
                oss << " bytes";
                emit_error(GCAP_OK, oss.str().c_str());
                logged_layout = true;
            }

            if (!ensure_upload_yuv(cur_w_, cur_h_))
            {
                if (locked2d)
                    buf2d->Unlock2D();
                else
                    buf->Unlock();
                continue;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hrMap = ctx_->Map(upload_yuv_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(hrMap))
            {
                if (locked2d)
                    buf2d->Unlock2D();
                else
                    buf->Unlock();
                MDBG("DXGI: Map(upload_yuv_) failed", hrMap);
                continue;
            }

            // bufferLen vs expected (upload path) + RowPitch
            if (!logged_len_mismatch)
            {
                const int assumeStride =
                    (locked2d && srcPitchLong > 0) ? (int)srcPitchLong : (cur_stride_ > 0)                  ? cur_stride_
                                                                     : (cur_subtype_ == MFVideoFormat_P010) ? (cur_w_ * 2)
                                                                                                            : cur_w_;

                const size_t expected = (size_t)assumeStride * (size_t)cur_h_ +
                                        (size_t)assumeStride * (size_t)(cur_h_ / 2);

                // 如果 curLen=0（常見於 2D buffer），就略過這個檢查
                if (curLen != 0 && (size_t)curLen < expected)
                {
                    std::ostringstream oss;
                    oss << "[WinMF] WARNING: bufferLen < expected (upload): curLen=" << curLen
                        << ", expected>=" << expected
                        << ", subtype=" << mf_subtype_name(cur_subtype_)
                        << ", w=" << cur_w_ << ", h=" << cur_h_
                        << ", default_stride=" << assumeStride
                        << ", upload_RowPitch=" << mapped.RowPitch;
                    emit_error(GCAP_OK, oss.str().c_str());
                    logged_len_mismatch = true;
                }
            }

            int w = cur_w_;
            int h = cur_h_;

            if (cur_subtype_ == MFVideoFormat_NV12)
            {
                const int srcStride = (locked2d && srcPitchLong > 0) ? (int)srcPitchLong : (cur_stride_ > 0) ? cur_stride_
                                                                                                             : w;
                const size_t rowBytes = (size_t)w; // NV12 Y/UV 每列 bytes = w

                const uint8_t *srcY = pData;
                const uint8_t *srcUV = pData + (size_t)srcStride * (size_t)h;

                // --- Recording: NV12 直接送進 Sink Writer (H.264) ---
                {
                    std::lock_guard<std::mutex> lock(recorderMutex_);
                    if (recorder_)
                    {
                        recorder_->writeNV12(srcY, srcUV,
                                             static_cast<UINT32>(srcStride),
                                             static_cast<UINT32>(srcStride),
                                             ts);
                    }
                }

                uint8_t *dst = static_cast<uint8_t *>(mapped.pData);

                // Y plane
                for (int y = 0; y < h; ++y)
                    memcpy(dst + mapped.RowPitch * y,
                           srcY + (size_t)srcStride * y,
                           rowBytes);
                // UV plane (h/2 行，pitch 相同)
                for (int y = 0; y < h / 2; ++y)
                    memcpy(dst + mapped.RowPitch * (h + y),
                           srcUV + (size_t)srcStride * y,
                           rowBytes);
            }
            else if (cur_subtype_ == MFVideoFormat_P010)
            {
                // P010: 10-bit，2 bytes per sample
                const int srcStride = (locked2d && srcPitchLong > 0) ? (int)srcPitchLong : (cur_stride_ > 0) ? cur_stride_
                                                                                                             : (w * 2);
                const size_t rowBytes = (size_t)w * 2;

                const uint8_t *srcY = pData;
                const uint8_t *srcUV = pData + (size_t)srcStride * (size_t)h;
                // --- Recording: P010 直接送進 Sink Writer (HEVC) ---
                {
                    std::lock_guard<std::mutex> lock(recorderMutex_);
                    if (recorder_)
                    {
                        recorder_->writeP010(srcY, srcUV,
                                             static_cast<UINT32>(srcStride),
                                             static_cast<UINT32>(srcStride),
                                             ts);
                    }
                }

                uint8_t *dst = static_cast<uint8_t *>(mapped.pData);

                for (int y = 0; y < h; ++y)
                    memcpy(dst + mapped.RowPitch * y,
                           srcY + (size_t)srcStride * y,
                           rowBytes);
                for (int y = 0; y < h / 2; ++y)
                    memcpy(dst + mapped.RowPitch * (h + y),
                           srcUV + (size_t)srcStride * y,
                           rowBytes);
            }
            else if (cur_subtype_ == MFVideoFormat_YUY2)
            {
                const int srcStride = (locked2d && srcPitchLong > 0) ? (int)srcPitchLong
                                                                     : (cur_stride_ > 0 ? cur_stride_ : (w * 2));
                const size_t rowBytes = (size_t)w * 2; // YUY2 每像素 2 bytes

                // 這裡的 upload texture 是「packed」：width = ceil(w/2)，RGBA8_UINT
                if (!ensure_upload_yuv(w, h))
                {
                    if (locked2d)
                        buf2d->Unlock2D();
                    else
                        buf->Unlock();
                    continue;
                }

                D3D11_MAPPED_SUBRESOURCE mapped{};
                HRESULT hrMap = ctx_->Map(upload_yuy2_packed_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                if (FAILED(hrMap))
                {
                    MDBG("DXGI: Map(upload_yuy2_packed_) failed", hrMap);
                    if (locked2d)
                        buf2d->Unlock2D();
                    else
                        buf->Unlock();
                    continue;
                }

                uint8_t *dst = (uint8_t *)mapped.pData;
                const int w2 = (w + 1) / 2;
                for (int yy = 0; yy < h; ++yy)
                {
                    const uint8_t *srcRow = pData + (size_t)srcStride * (size_t)yy;
                    uint8_t *dstRow = dst + (size_t)mapped.RowPitch * (size_t)yy;

                    // 每 4 bytes（Y0 U Y1 V）→ 寫成 1 個 RGBA8_UINT texel
                    for (int x = 0; x < w2; ++x)
                    {
                        const int srcX = x * 4;
                        // 注意：若 w 是奇數，最後一組的 Y1 可能不存在，這裡用 Y0 補
                        uint8_t Y0 = srcRow[srcX + 0];
                        uint8_t U = srcRow[srcX + 1];
                        uint8_t Y1 = (srcX + 2 < (int)rowBytes) ? srcRow[srcX + 2] : Y0;
                        uint8_t V = (srcX + 3 < (int)rowBytes) ? srcRow[srcX + 3] : srcRow[srcX + 1];

                        uint8_t *d4 = dstRow + x * 4;
                        d4[0] = Y0;
                        d4[1] = U;
                        d4[2] = Y1;
                        d4[3] = V;
                    }
                }

                ctx_->Unmap(upload_yuy2_packed_.Get(), 0);
                if (locked2d)
                    buf2d->Unlock2D();
                else
                    buf->Unlock();

                yuvTex = upload_yuy2_packed_.Get();
            }

            if (cur_subtype_ == MFVideoFormat_NV12 || cur_subtype_ == MFVideoFormat_P010)
            {
                ctx_->Unmap(upload_yuv_.Get(), 0);
                if (locked2d)
                    buf2d->Unlock2D();
                else
                    buf->Unlock();

                yuvTex = upload_yuv_.Get();
            }
        }

        if (!yuvTex)
            continue;

        if (!render_yuv_to_rgba(yuvTex.Get()))
        {
            MDBG("DXGI: render_yuv_to_rgba failed", E_FAIL);
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
            : (cur_subtype_ == MFVideoFormat_YUY2)   ? L"YUY2"
            : (cur_subtype_ == MFVideoFormat_ARGB32) ? L"ARGB32"
                                                     : L"?";

        const wchar_t *bitDepth =
            (cur_subtype_ == MFVideoFormat_P010) ? L"10-bit"
                                                 : L"8-bit"; // NV12 / ARGB32 皆 8-bit

        double fps_show = fps_avg_ > 0.0 ? fps_avg_ : 0.0;

        const wchar_t *gpuName =
            (!gpu_name_w_.empty() ? gpu_name_w_.c_str() : L"(GPU: unknown)");

        wchar_t line[512];
        swprintf(line,
                 512,
                 L"%s | GPU: %s | %dx%d @ %.2f fps | %s %s | #%llu",
                 (wdev[0] ? wdev : L"Device"),
                 gpuName,
                 cur_w_,
                 cur_h_,
                 fps_show,
                 fmtName,
                 bitDepth,
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

    if (cur_subtype_ == MFVideoFormat_YUY2)
    {
        return ensure_upload_yuy2_packed(w, h);
    }

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
        MDBG("DXGI: Create upload NV12 texture failed", hr);
        return false;
    }
    return true;
}

bool WinMFProvider::ensure_upload_yuy2_packed(int w, int h)
{
    if (!d3d_)
        return false;

    const int w2 = (w + 1) / 2;
    if (upload_yuy2_packed_)
    {
        D3D11_TEXTURE2D_DESC desc{};
        upload_yuy2_packed_->GetDesc(&desc);
        if ((int)desc.Width == w2 && (int)desc.Height == h && desc.Format == DXGI_FORMAT_R8G8B8A8_UINT)
            return true;
        upload_yuy2_packed_.Reset();
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w2;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UINT; // 給 Texture2D<uint4>.Load 用
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    td.MiscFlags = 0;

    HRESULT hr = d3d_->CreateTexture2D(&td, nullptr, &upload_yuy2_packed_);
    if (FAILED(hr))
    {
        MDBG("DXGI: Create upload YUY2 packed texture failed", hr);
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
        MDBG("DXGI: D3DCompile(g_cs_nv12) failed", hr);
        return false;
    }

    hr = d3d_->CreateComputeShader(
        csb->GetBufferPointer(),
        csb->GetBufferSize(),
        nullptr,
        &cs_nv12_);
    if (FAILED(hr))
    {
        MDBG("DXGI: CreateComputeShader(cs_nv12_) failed", hr);
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
        MDBG("DXGI: CreateBuffer(cs_params_) failed", hr);
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
            MDBG("DXGI: CreateUnorderedAccessView(rt_rgba_) failed", hr);
            return false;
        }
    }

    // 更新 constant buffer（寬、高）
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hrMap = ctx_->Map(cs_params_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hrMap))
    {
        MDBG("DXGI: Map(cs_params_) failed", hrMap);
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
