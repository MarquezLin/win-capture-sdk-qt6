// Split out from winmf_provider.cpp (recording layer)

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "mf_recorder.h"

#include <mferror.h>

#include <comdef.h>
#include <windows.h>
#include <ksmedia.h>

#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sstream>

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

// ------------------------------
// WasapiCapture
// ------------------------------

WasapiCapture::WasapiCapture() = default;

WasapiCapture::~WasapiCapture()
{
    stop();
}

bool WasapiCapture::start(UINT32 sampleRate, UINT32 channels, UINT32 bits,
                          const std::wstring &endpointId, ActualFormat *outFmt)
{
    stop();
    // OBS-style: local cursor timeline (do not trust devPos)
    tsCursor100ns_ = 0;

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

void WasapiCapture::stop()
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

bool WasapiCapture::pop(Chunk &out)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (queue_.empty())
        return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

bool WasapiCapture::waitForData(int timeoutMs)
{
    std::unique_lock<std::mutex> lk(mutex_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]()
                      { return !queue_.empty() || !running_.load(); }))
        return false;
    return !queue_.empty();
}

static bool parse_mix_format(WAVEFORMATEX *wfex, WasapiCapture::CaptureFormat &out)
{
    if (!wfex)
        return false;

    out.sampleRate = wfex->nSamplesPerSec;
    out.channels = wfex->nChannels;
    out.blockAlign = wfex->nBlockAlign;

    out.bits = wfex->wBitsPerSample;
    out.isFloat = false;

    if (wfex->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        out.isFloat = true;
    else if (wfex->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        auto *wfe = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(wfex);
        if (wfe->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            out.isFloat = true;
        out.bits = wfe->Format.wBitsPerSample;
    }
    return true;
}

// Convert float32 or int32 mix format to PCM16 interleaved
static void mix_to_pcm16(const BYTE *src, UINT32 frames,
                         const WasapiCapture::CaptureFormat &cap,
                         std::vector<uint8_t> &dst)
{
    const UINT32 ch = cap.channels;
    dst.resize((size_t)frames * ch * 2);
    int16_t *out = reinterpret_cast<int16_t *>(dst.data());

    if (cap.isFloat)
    {
        const float *in = reinterpret_cast<const float *>(src);
        const size_t n = (size_t)frames * ch;
        for (size_t i = 0; i < n; ++i)
        {
            float v = in[i];
            if (v > 1.0f)
                v = 1.0f;
            if (v < -1.0f)
                v = -1.0f;
            out[i] = (int16_t)lrintf(v * 32767.0f);
        }
        return;
    }

    // 16-bit PCM already
    if (cap.bits == 16)
    {
        memcpy(dst.data(), src, dst.size());
        return;
    }

    // 32-bit PCM
    if (cap.bits == 32)
    {
        const int32_t *in = reinterpret_cast<const int32_t *>(src);
        const size_t n = (size_t)frames * ch;
        for (size_t i = 0; i < n; ++i)
            out[i] = (int16_t)(in[i] >> 16);
        return;
    }

    // Fallback: silence
    memset(dst.data(), 0, dst.size());
}

void WasapiCapture::run()
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
        // We always output PCM16 to the upper layer (OBS-style stability)
        actual_.bits = (isFloat_ && bits_ == 32) ? 16 : bits_;
        actual_.isFloat = false;
        actual_.blockAlign = actual_.channels * (actual_.bits / 8);
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
            // OBS-style: build timeline from local cursor; do not trust devPos (Bluetooth devices may jump).
            const UINT32 bytesPerFrame = (blockAlign_ != 0) ? blockAlign_ : (channels_ * (bits_ / 8));
            const UINT32 bytesIn = frames * bytesPerFrame;

            Chunk ck;
            ck.ts100ns = tsCursor100ns_;
            ck.dur100ns = (LONGLONG)frames * 10'000'000LL / (LONGLONG)sampleRate_;

            // Output is always PCM16 (even if engine gives float32)
            const UINT32 outBytesPerFrame = channels_ * 2;
            const UINT32 bytesOut = frames * outBytesPerFrame;
            ck.pcm.resize(bytesOut);

            if (flags2 & AUDCLNT_BUFFERFLAGS_SILENT || !data)
            {
                memset(ck.pcm.data(), 0, bytesOut);
            }
            else
            {
                if (isFloat_ && bits_ == 32)
                {
                    // interleaved float32 [-1,1] -> PCM16
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
                    // assume PCM16 compatible
                    memcpy(ck.pcm.data(), data, bytesOut);
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
            tsCursor100ns_ += ck.dur100ns;

            hr = captureClient_->GetNextPacketSize(&packet);
            if (FAILED(hr))
                break;
        }
    }

    audioClient_->Stop();
    CoUninitialize();
}

// ------------------------------
// WinMFProvider::MfRecorder
// ------------------------------

void WinMFProvider::MfRecorder::stopAudioThread()
{
    audioRunning.store(false);
    wasapi.stop();
    if (audioThread.joinable())
        audioThread.join();
}

void WinMFProvider::MfRecorder::close()
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

bool WinMFProvider::MfRecorder::writeOneAudioSample(LONGLONG ts100ns, LONGLONG dur100ns, const uint8_t *data, DWORD bytes)
{
    if (!writer)
        return false;

    ComPtr<IMFSample> s;
    HRESULT hr = MFCreateSample(&s);
    if (FAILED(hr))
        return false;

    ComPtr<IMFMediaBuffer> b;
    hr = MFCreateMemoryBuffer(bytes, &b);
    if (FAILED(hr))
        return false;

    BYTE *dst = nullptr;
    DWORD maxLen = 0;
    hr = b->Lock(&dst, &maxLen, nullptr);
    if (FAILED(hr) || !dst || maxLen < bytes)
        return false;

    memcpy(dst, data, bytes);
    b->Unlock();
    b->SetCurrentLength(bytes);

    hr = s->AddBuffer(b.Get());
    if (FAILED(hr))
        return false;

    s->SetSampleTime(ts100ns);
    s->SetSampleDuration(dur100ns);

    std::lock_guard<std::mutex> lk(writerMutex);
    hr = writer->WriteSample(audioStreamIndex, s.Get());
    if (FAILED(hr))
    {
        OutputDebugStringA(("[WinMF][Audio] WriteSample failed: " + hr_msg(hr) + "\\n").c_str());
        return false;
    }
    return true;
}

bool WinMFProvider::MfRecorder::writeAudioDrainOnce()
{
    if (!writer || !hasAudio)
        return true;

    const UINT32 bytesPerSample = audioBits / 8;
    const UINT32 blockAlign = (audioBlockAlign != 0) ? audioBlockAlign : (audioChannels * bytesPerSample);

    const UINT32 frameSamples = audioSampleRate / 50; // 20ms @ audioSampleRate
    const UINT32 frameBytes = frameSamples * blockAlign;
    const LONGLONG frameDur100ns = (LONGLONG)frameSamples * 10'000'000LL / (LONGLONG)audioSampleRate;

    // Limit work per call so audio thread won't hog CPU
    const int kMaxChunksPerCall = 32;

    WasapiCapture::Chunk ck;
    int processed = 0;
    while (processed < kMaxChunksPerCall && wasapi.pop(ck))
    {
        processed++;

        // We ignore ck.ts100ns from device and build a continuous timeline by consumed samples (OBS-style).
        // Append bytes
        if (!ck.pcm.empty())
        {
            audioAccum.insert(audioAccum.end(), ck.pcm.begin(), ck.pcm.end());
        }

        // Emit fixed frames
        while (audioAccum.size() >= frameBytes)
        {
            if (!writeOneAudioSample(audioPtsCursor100ns, frameDur100ns, audioAccum.data(), frameBytes))
                return false;

            audioPtsCursor100ns += frameDur100ns;
            audioAccum.erase(audioAccum.begin(), audioAccum.begin() + frameBytes);
        }
    }

    return true;
}

bool WinMFProvider::MfRecorder::open(const std::wstring &path, UINT32 w, UINT32 h,
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
        // ------------------------------
        // Audio (OBS-style):
        //  - Start WASAPI first (get actual device format)
        //  - Assemble fixed 20ms frames (in audio thread) before writing to SinkWriter
        // ------------------------------
        hasAudio = false;
        audioAccum.clear();
        audioPtsCursor100ns = 0;

        WasapiCapture::ActualFormat af{};
        if (wasapi.start(audioSampleRate, audioChannels, audioBits, audioEndpointIdW, &af))
        {
            hasAudio = true;
            audioSampleRate = af.sampleRate ? af.sampleRate : audioSampleRate;
            audioChannels = af.channels ? af.channels : audioChannels;
            audioBits = af.bits ? af.bits : audioBits; // WasapiCapture guarantees PCM16 output
            audioIsFloat = false;
            audioBlockAlign = af.blockAlign ? af.blockAlign : (audioChannels * (audioBits / 8));
        }

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

        if (hasAudio)
        {
            audioStreamIndex = aidx;
        }
    }

    hr = wtr->BeginWriting();
    if (FAILED(hr))
        return false;

    writer = wtr;
    streamIndex = idx;
    firstTs100ns = -1;
    lastAudioTs100ns = 0;

    // WASAPI already started above (before media-type setup)

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

bool WinMFProvider::MfRecorder::writePlanar(const uint8_t *y, const uint8_t *uv,
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
        // Duration = 1 frame
        if (fpsNum != 0)
        {
            const LONGLONG vDuration = (10'000'000LL * (LONGLONG)fpsDen) / (LONGLONG)fpsNum;
            sample->SetSampleDuration(vDuration);
        }
    }

    std::lock_guard<std::mutex> lk(writerMutex);
    hr = writer->WriteSample(streamIndex, sample.Get());
    if (FAILED(hr))
    {
        OutputDebugStringA(("[WinMF][Rec] video WriteSample failed: " + hr_msg(hr) + "\\n").c_str());
        return false;
    }

    return true;
}

bool WinMFProvider::MfRecorder::writeNV12(const uint8_t *y, const uint8_t *uv,
                                          UINT32 yStride, UINT32 uvStride,
                                          LONGLONG ts100ns)
{
    if (isP010)
        return false;
    return writePlanar(y, uv, yStride, uvStride, ts100ns);
}

bool WinMFProvider::MfRecorder::writeP010(const uint8_t *y, const uint8_t *uv,
                                          UINT32 yStrideBytes, UINT32 uvStrideBytes,
                                          LONGLONG ts100ns)
{
    if (!isP010)
        return false;
    return writePlanar(y, uv, yStrideBytes, uvStrideBytes, ts100ns);
}