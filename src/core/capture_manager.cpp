#include "capture_manager.h"
#include <cstring>

#ifdef GCAP_WIN_MF
#include "../providers/winmf_provider.h"
#endif

/**
 * @brief Constructor — selects platform-specific provider.
 */
CaptureManager::CaptureManager()
{
#ifdef GCAP_WIN_MF
    // Use Media Foundation provider on Windows
    provider_ = std::make_unique<WinMFProvider>();
#else
    // Unsupported on this platform (no provider defined)
    provider_ = nullptr;
#endif
}

/**
 * @brief Destructor — ensures the device is properly closed.
 */
CaptureManager::~CaptureManager()
{
    close();
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
