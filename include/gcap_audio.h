#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _WIN32
#ifdef GCAPTURE_BUILD
#define GCAP_API __declspec(dllexport)
#else
#define GCAP_API __declspec(dllimport)
#endif
#else
#define GCAP_API
#endif

#define GCAP_AUDIO_ID_MAX 256
#define GCAP_AUDIO_NAME_MAX 256

    typedef struct gcap_audio_device_t
    {
        char id[GCAP_AUDIO_ID_MAX];     // opaque WASAPI device id
        char name[GCAP_AUDIO_NAME_MAX]; // friendly name

        int channels;        // 1, 2, 6...
        int sample_rate;     // 44100, 48000...
        int bits_per_sample; // 16 / 24 / 32
        int is_float;        // 1 = IEEE float, 0 = PCM
        int is_default;
    } gcap_audio_device_t;

    // number of active capture audio devices
    GCAP_API int gcap_get_audio_device_count(void);

    // enumerate capture audio devices
    // return: number written (or total count if out == NULL)
    GCAP_API int gcap_enum_audio_devices(
        gcap_audio_device_t *out,
        int max_count);

    typedef struct gcap_audio_capture_config_t
    {
        const char *device_id; // 來自 Step 2 選到的 id
        int sample_rate;       // 建議 48000
        int channels;          // 1 or 2
    } gcap_audio_capture_config_t;

    // 開始 audio capture（for recording）
    GCAP_API int gcap_start_audio_capture(
        const gcap_audio_capture_config_t *cfg);

    // 停止 audio capture
    GCAP_API void gcap_stop_audio_capture(void);

#ifdef __cplusplus
}
#endif
