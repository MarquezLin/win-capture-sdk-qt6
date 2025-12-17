#pragma once
#include <stdint.h>

#ifdef _WIN32
#ifdef GCAPTURE_BUILD
#define GCAP_API __declspec(dllexport)
#else
#define GCAP_API __declspec(dllimport)
#endif
#else
#define GCAP_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        GCAP_BACKEND_WINMF_CPU = 0,
        GCAP_BACKEND_WINMF_GPU = 1,
        GCAP_BACKEND_DSHOW = 2
    } gcap_backend_t;

    enum gcap_profile_mode_t
    {
        GCAP_PROFILE_DEVICE_DEFAULT = 0,
        GCAP_PROFILE_CUSTOM
    };

    typedef enum
    {
        GCAP_OK = 0,
        GCAP_EINVAL,
        GCAP_ENODEV,
        GCAP_ESTATE,
        GCAP_EIO,
        GCAP_ENOTSUP
    } gcap_status_t;

    typedef enum
    {
        GCAP_FMT_NV12,
        GCAP_FMT_YUY2,
        GCAP_FMT_ARGB,
        GCAP_FMT_P010,
        GCAP_FMT_V210,
        GCAP_FMT_R210
    } gcap_pixfmt_t;

    typedef struct
    {
        int index;
        char name[128];
        char symbolic_link[256];
        unsigned caps; // bitmask: 1<<0:HDMI,1<<1:SDI,1<<2:BIT10...
    } gcap_device_info_t;

    typedef struct
    {
        int width, height;
        int fps_num, fps_den;
        gcap_pixfmt_t format;
        gcap_profile_mode_t mode;
    } gcap_profile_t;

    typedef struct
    {
        const void *data[3];
        int stride[3];
        int plane_count;
        int width, height;
        gcap_pixfmt_t format;
        uint64_t pts_ns;
        uint64_t frame_id;
    } gcap_frame_t;

    typedef void (*gcap_on_video_cb)(const gcap_frame_t *frame, void *user);
    typedef void (*gcap_on_error_cb)(gcap_status_t code, const char *msg, void *user);

    typedef struct gcap_handle_t *gcap_handle;

    gcap_status_t gcap_enumerate(gcap_device_info_t *out, int max, int *count);
    gcap_status_t gcap_open(int device_index, gcap_handle *out);
    gcap_status_t gcap_set_profile(gcap_handle h, const gcap_profile_t *prof);
    gcap_status_t gcap_set_buffers(gcap_handle h, int count, size_t bytes_hint);
    gcap_status_t gcap_set_callbacks(gcap_handle h, gcap_on_video_cb vcb, gcap_on_error_cb ecb, void *user);
    gcap_status_t gcap_start(gcap_handle h);
    gcap_status_t gcap_start_recording(gcap_handle h, const char *path_utf8);
    gcap_status_t gcap_stop_recording(gcap_handle h);
    gcap_status_t gcap_stop(gcap_handle h);
    gcap_status_t gcap_close(gcap_handle h);
    GCAP_API void gcap_set_backend(int backend);
    // 選擇要用哪一張 D3D11 Adapter 來做 NV12→RGBA / DXGI 管線
    // adapter_index = -1 表示使用系統預設（原本的 nullptr / default adapter）
    GCAP_API void gcap_set_d3d_adapter(int adapter_index);

    const char *gcap_strerror(gcap_status_t);

#ifdef __cplusplus
}
#endif
