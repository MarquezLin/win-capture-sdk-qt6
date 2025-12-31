#pragma once

// Recording layer extracted from winmf_provider.cpp
// - WasapiCapture helper (WASAPI endpoint capture into a queue)
// - WinMFProvider::MfRecorder (Media Foundation Sink Writer recorder)

// Need the full WinMFProvider declaration (the nested MfRecorder is declared there).
#include "winmf_provider.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ------------------------------------------------------------
// WASAPI capture (old stable behavior)
//  - Shared mode
//  - Event-driven capture
//  - Prefer requested PCM format; fallback to mix format
//  - Enable engine conversion (AUTOCONVERTPCM + SRC_DEFAULT_QUALITY)
//  - Output is ALWAYS PCM16 to the upper layer (OBS-style stability)
// ------------------------------------------------------------
class WasapiCapture
{
public:
    struct Chunk
    {
        LONGLONG ts100ns = 0;     // relative timeline
        LONGLONG dur100ns = 0;    // duration
        std::vector<uint8_t> pcm; // interleaved bytes (PCM16)
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

    // Special members are defined in mf_recorder.cpp, so they must be declared here.
    WasapiCapture();
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture &) = delete;
    WasapiCapture &operator=(const WasapiCapture &) = delete;
    WasapiCapture(WasapiCapture &&) = delete;
    WasapiCapture &operator=(WasapiCapture &&) = delete;

    bool start(UINT32 sampleRate, UINT32 channels, UINT32 bits,
               const std::wstring &endpointId, ActualFormat *outFmt = nullptr);
    void stop();

    // non-blocking pop
    bool pop(Chunk &out);

    // wait until queue has data or stopped (does NOT consume)
    bool waitForData(int timeoutMs);

private:
    void run();

    std::atomic<bool> running_{false};
    std::thread thread_;
    HANDLE event_ = nullptr;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<IMMDevice> dev_;
    Microsoft::WRL::ComPtr<IAudioClient> audioClient_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient_;

    UINT32 sampleRate_ = 48000;
    UINT32 channels_ = 2;
    UINT32 bits_ = 16;
    std::wstring endpointId_;
    bool isFloat_ = false;
    UINT32 blockAlign_ = 0;
    CaptureFormat cap_{};

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

// Media Foundation Sink Writer recorder (NV12->H.264, P010->HEVC) extracted.
// NOTE: This is still a nested type of WinMFProvider.
struct WinMFProvider::MfRecorder
{
    Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
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
    bool isP010 = false;        // false: NV12 -> H.264, true: P010 -> HEVC
    LONGLONG firstTs100ns = -1; // first video ts as 0

    WasapiCapture wasapi; // WASAPI capture + queue

    // audio timeline state (relative 100ns, 0-based)
    LONGLONG lastAudioTs100ns = 0;
    std::vector<uint8_t> audioAccum;  // PCM16 bytes accumulator
    LONGLONG audioPtsCursor100ns = 0; // continuous audio PTS (OBS-style)

    void stopAudioThread();
    void close();

    bool open(const std::wstring &path,
              UINT32 w, UINT32 h,
              UINT32 fpsN, UINT32 fpsD,
              bool p010,
              const std::wstring &audioEndpointIdW);

    bool writeNV12(const uint8_t *y, const uint8_t *uv,
                   UINT32 yStride, UINT32 uvStride,
                   LONGLONG ts100ns);

    bool writeP010(const uint8_t *y, const uint8_t *uv,
                   UINT32 yStrideBytes, UINT32 uvStrideBytes,
                   LONGLONG ts100ns);

private:
    bool writeOneAudioSample(LONGLONG ts100ns, LONGLONG dur100ns, const uint8_t *data, DWORD bytes);
    bool writeAudioDrainOnce();
    bool writePlanar(const uint8_t *y, const uint8_t *uv,
                     UINT32 yStrideBytes, UINT32 uvStrideBytes,
                     LONGLONG ts100ns);
};
