// src/core/c_api.cpp
#include "capture_manager.h"
#ifndef GCAPTURE_BUILD
#error not exporting
#endif
#include "gcapture.h"
#include <memory>
#include <vector>
#include "../audio/audio_manager.h"
#include "gcap_audio.h"

#ifdef _WIN32
#include <windows.h>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#pragma comment(lib, "ole32.lib")
using Microsoft::WRL::ComPtr;

static std::string w2utf8(const wchar_t *ws)
{
    if (!ws)
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string out((size_t)len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), len, nullptr, nullptr);
    return out;
}
#endif

extern "C"
{
    // 簡單的 handle 物件，內含一個 CaptureManager
    struct gcap_handle_t
    {
        CaptureManager mgr;
    };

    // 錯誤字串
    const char *gcap_strerror(gcap_status_t s)
    {
        switch (s)
        {
        case GCAP_OK:
            return "OK";
        case GCAP_EINVAL:
            return "Invalid argument";
        case GCAP_ENODEV:
            return "No such device";
        case GCAP_ESTATE:
            return "Invalid state";
        case GCAP_EIO:
            return "I/O error";
        case GCAP_ENOTSUP:
            return "Not supported";
        default:
            return "Unknown";
        }
    }

    // 只為了列舉裝置，不需要長壽命 handle
    gcap_status_t gcap_enumerate(gcap_device_info_t *out, int max, int *count)
    {
        if (!out || max <= 0)
            return GCAP_EINVAL;
        CaptureManager tmp;
        return tmp.enumerate(out, max, count);
    }

    gcap_status_t gcap_open(int device_index, gcap_handle *out)
    {
        if (!out)
            return GCAP_EINVAL;
        auto h = std::make_unique<gcap_handle_t>();
        gcap_status_t st = h->mgr.open(device_index);
        if (st != GCAP_OK)
            return st;
        *out = h.release();
        return GCAP_OK;
    }

    gcap_status_t gcap_set_profile(gcap_handle h, const gcap_profile_t *p)
    {
        if (!h || !p)
            return GCAP_EINVAL;
        return h->mgr.setProfile(*p);
    }

    gcap_status_t gcap_set_buffers(gcap_handle h, int count, size_t bytes_hint)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.setBuffers(count, bytes_hint);
    }

    gcap_status_t gcap_set_callbacks(gcap_handle h,
                                     gcap_on_video_cb vcb,
                                     gcap_on_error_cb ecb,
                                     void *user)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.setCallbacks(vcb, ecb, user);
    }

    gcap_status_t gcap_start(gcap_handle h)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.start();
    }

    gcap_status_t gcap_start_recording(gcap_handle h, const char *path_utf8)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.startRecording(path_utf8);
    }

    gcap_status_t gcap_stop_recording(gcap_handle h)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.stopRecording();
    }

    GCAP_API gcap_status_t gcap_enumerate_audio_devices(gcap_audio_device_t *out, int max, int *count)
    {
        if (!out || max <= 0)
            return GCAP_EINVAL;

#ifndef _WIN32
        if (count)
            *count = 0;
        return GCAP_ENOTSUP;
#else
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        ComPtr<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr))
        {
            if (count)
                *count = 0;
            CoUninitialize();
            return GCAP_EIO;
        }

        // Default endpoint id
        std::wstring defaultId;
        {
            ComPtr<IMMDevice> def;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &def)))
            {
                wchar_t *wid = nullptr;
                if (SUCCEEDED(def->GetId(&wid)) && wid)
                {
                    defaultId = wid;
                    CoTaskMemFree(wid);
                }
            }
        }

        ComPtr<IMMDeviceCollection> coll;
        hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll);
        if (FAILED(hr))
        {
            if (count)
                *count = 0;
            CoUninitialize();
            return GCAP_EIO;
        }

        UINT n = 0;
        coll->GetCount(&n);
        if (count)
            *count = (int)n;

        UINT toCopy = (UINT)max;
        if (toCopy > n)
            toCopy = n;

        for (UINT i = 0; i < toCopy; ++i)
        {
            ComPtr<IMMDevice> dev;
            if (FAILED(coll->Item(i, &dev)))
                continue;

            // id
            wchar_t *wid = nullptr;
            std::wstring idW;
            if (SUCCEEDED(dev->GetId(&wid)) && wid)
            {
                idW = wid;
                CoTaskMemFree(wid);
            }

            // name
            std::wstring nameW;
            ComPtr<IPropertyStore> store;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)))
            {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)))
                {
                    if (pv.vt == VT_LPWSTR && pv.pwszVal)
                        nameW = pv.pwszVal;
                }
                PropVariantClear(&pv);
            }

            std::string idU8 = w2utf8(idW.c_str());
            std::string nameU8 = w2utf8(nameW.c_str());

            memset(&out[i], 0, sizeof(out[i]));
            strncpy(out[i].id, idU8.c_str(), sizeof(out[i].id) - 1);
            strncpy(out[i].name, nameU8.c_str(), sizeof(out[i].name) - 1);
            out[i].is_default = (!defaultId.empty() && idW == defaultId) ? 1 : 0;
        }

        CoUninitialize();
        return GCAP_OK;
#endif
    }

    GCAP_API gcap_status_t gcap_set_recording_audio_device(gcap_handle h, const char *device_id_utf8)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.setRecordingAudioDevice(device_id_utf8);
    }

    gcap_status_t gcap_stop(gcap_handle h)
    {
        if (!h)
            return GCAP_EINVAL;
        return h->mgr.stop();
    }

    gcap_status_t gcap_close(gcap_handle h)
    {
        if (!h)
            return GCAP_EINVAL;
        // 先停再關（容錯）
        h->mgr.stop();
        gcap_status_t st = h->mgr.close();
        delete h;
        return st;
    }

    gcap_status_t gcap_get_device_props(gcap_handle h, gcap_device_props_t *out)
    {
        if (!h || !out)
            return GCAP_EINVAL;
        return h->mgr.getDeviceProps(*out);
    }

    gcap_status_t gcap_get_signal_status(gcap_handle h, gcap_signal_status_t *out)
    {
        if (!h || !out)
            return GCAP_EINVAL;
        return h->mgr.getSignalStatus(*out);
    }

    gcap_status_t gcap_set_processing(gcap_handle h, const gcap_processing_opts_t *opts)
    {
        if (!h || !opts)
            return GCAP_EINVAL;
        return h->mgr.setProcessing(*opts);
    }

    GCAP_API void gcap_set_backend(int backend)
    {
        CaptureManager::setBackendInt(backend);
    }

    GCAP_API void gcap_set_d3d_adapter(int adapter_index)
    {
        CaptureManager::setD3dAdapterInt(adapter_index);
    }

    extern "C" GCAP_API int gcap_get_audio_device_count(void)
    {
        auto list = gcap::audio::enumerate_devices();
        return static_cast<int>(list.size());
    }

    extern "C" GCAP_API int gcap_enum_audio_devices(
        gcap_audio_device_t *out,
        int max_count)
    {
        auto list = gcap::audio::enumerate_devices();
        int total = static_cast<int>(list.size());

        if (!out || max_count <= 0)
            return total;

        int n = (total < max_count) ? total : max_count;

        for (int i = 0; i < n; ++i)
        {
            const auto &d = list[i];

            memset(&out[i], 0, sizeof(gcap_audio_device_t));
            strncpy_s(out[i].id, d.id.c_str(), GCAP_AUDIO_ID_MAX - 1);
            strncpy_s(out[i].name, d.name.c_str(), GCAP_AUDIO_NAME_MAX - 1);
            out[i].channels = d.channels;
            out[i].sample_rate = d.sample_rate;
            out[i].bits_per_sample = d.bits_per_sample;
            out[i].is_float = d.is_float ? 1 : 0;
        }

        return n;
    }

} // extern "C"
