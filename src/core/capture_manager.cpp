#include "capture_manager.h"
#include <cstring>

#ifdef GCAP_WIN_MF
#include "../providers/winmf_provider.h"
#endif

#ifdef GCAP_WIN_DSHOW
#include "../providers/dshow_provider.h"
#endif

enum class Backend
{
    WinMF_CPU,
    WinMF_GPU,
    DShow
};
static Backend g_backend = Backend::WinMF_GPU;

// 預設 D3D Adapter (-1 = 由系統選擇 default adapter)
static int g_d3d_adapter_index = -1;

/**
 * @brief Constructor — selects platform-specific provider.
 */
CaptureManager::CaptureManager()
{
    switch (g_backend)
    {
    case Backend::DShow:
        provider_ = std::make_unique<DShowProvider>();
        break;
    case Backend::WinMF_CPU:
    case Backend::WinMF_GPU:
    default:
        provider_ = std::make_unique<WinMFProvider>(
            g_backend == Backend::WinMF_GPU // 傳一個 preferGpu flag
        );
        break;
    }
}

/**
 * @brief Destructor — ensures the device is properly closed.
 */
CaptureManager::~CaptureManager()
{
    close();
}

void CaptureManager::setBackendInt(int v)
{
    switch (v)
    {
    case 0:
        g_backend = Backend::WinMF_CPU;
        break;
    case 1:
        g_backend = Backend::WinMF_GPU;
        break;
    case 2:
    default:
        g_backend = Backend::DShow;
        break;
    }
}

void CaptureManager::setD3dAdapterInt(int index)
{
    g_d3d_adapter_index = index;

#ifdef GCAP_WIN_MF
    // 告訴 WinMFProvider 要改用哪一張 GPU
    WinMFProvider::setPreferredAdapterIndex(index);
#else
    (void)index;
#endif
}

/**
 * @brief Enumerate all capture devices.
 * @return GCAP_OK if successful, or error code otherwise.
 */
gcap_status_t CaptureManager::enumerate(gcap_device_info_t *out, int max, int *count)
{
    if (!provider_)
        return GCAP_ENOTSUP; // Not supported on this platform
    std::vector<gcap_device_info_t> list;
    if (!provider_->enumerate(list))
        return GCAP_EIO;
    int n = (int)list.size();
    if (count)
        *count = n;
    for (int i = 0; i < n && i < max; ++i)
        out[i] = list[i];
    return GCAP_OK;
}

/**
 * @brief Open the selected device.
 */
gcap_status_t CaptureManager::open(int idx)
{
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->open(idx) ? GCAP_OK : GCAP_EIO;
}

/**
 * @brief Set the desired capture profile (resolution, FPS, format).
 */
gcap_status_t CaptureManager::setProfile(const gcap_profile_t &p)
{
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->setProfile(p) ? GCAP_OK : GCAP_EINVAL;
}

/**
 * @brief Configure capture buffers.
 */
gcap_status_t CaptureManager::setBuffers(int c, size_t b)
{
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->setBuffers(c, b) ? GCAP_OK : GCAP_EINVAL;
}

/**
 * @brief Register video and error callbacks.
 */
gcap_status_t CaptureManager::setCallbacks(gcap_on_video_cb v, gcap_on_error_cb e, void *u)
{
    vcb_ = v;
    ecb_ = e;
    user_ = u;
    if (!provider_)
        return GCAP_ENOTSUP;
    provider_->setCallbacks(vcb_, ecb_, user_);
    return GCAP_OK;
}

/**
 * @brief Start video capture.
 */
gcap_status_t CaptureManager::start()
{
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->start() ? GCAP_OK : GCAP_ESTATE;
}

/**
 * @brief Stop video capture.
 */
gcap_status_t CaptureManager::stop()
{
    if (!provider_)
        return GCAP_ENOTSUP;
    provider_->stop();
    return GCAP_OK;
}

gcap_status_t CaptureManager::startRecording(const char *pathUtf8)
{
    if (!provider_)
        return GCAP_ENOTSUP;

#ifdef GCAP_WIN_MF
    if (auto *p = dynamic_cast<WinMFProvider *>(provider_.get()))
        return p->startRecording(pathUtf8);
#endif
    return GCAP_ENOTSUP;
}

gcap_status_t CaptureManager::stopRecording()
{
    if (!provider_)
        return GCAP_ENOTSUP;

#ifdef GCAP_WIN_MF
    if (auto *p = dynamic_cast<WinMFProvider *>(provider_.get()))
        return p->stopRecording();
#endif
    return GCAP_ENOTSUP;
}

gcap_status_t CaptureManager::setRecordingAudioDevice(const char *deviceIdUtf8)
{
    if (!provider_)
        return GCAP_ENOTSUP;

#ifdef GCAP_WIN_MF
    if (auto *p = dynamic_cast<WinMFProvider *>(provider_.get()))
        return p->setRecordingAudioDevice(deviceIdUtf8);
#endif
    (void)deviceIdUtf8;
    return GCAP_ENOTSUP;
}

/**
 * @brief Close the current device and release resources.
 */
gcap_status_t CaptureManager::close()
{
    if (!provider_)
        return GCAP_ENOTSUP;
    provider_->close();
    return GCAP_OK;
}

gcap_status_t CaptureManager::getDeviceProps(gcap_device_props_t &out)
{
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->getDeviceProps(out) ? GCAP_OK : GCAP_ENOTSUP;
}

gcap_status_t CaptureManager::getSignalStatus(gcap_signal_status_t &out)
{
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->getSignalStatus(out) ? GCAP_OK : GCAP_ENOTSUP;
}

gcap_status_t CaptureManager::setProcessing(const gcap_processing_opts_t &opts)
{
    if (!provider_)
        return GCAP_ENOTSUP;
    return provider_->setProcessing(opts) ? GCAP_OK : GCAP_ENOTSUP;
}
