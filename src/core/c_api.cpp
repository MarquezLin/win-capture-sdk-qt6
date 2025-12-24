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
