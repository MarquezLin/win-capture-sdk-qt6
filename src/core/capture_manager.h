#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include "gcapture.h"

/**
 * @brief Abstract interface for all capture providers.
 *
 * This interface defines the common functions required for
 * any video capture backend implementation (e.g. Media Foundation, V4L2, DeckLink).
 */
struct ICaptureProvider
{
    virtual ~ICaptureProvider() = default;
    /**
     * @brief Enumerate all available capture devices in the system.
     * @param list Output vector containing device information.
     * @return true if successful, false otherwise.
     */
    virtual bool enumerate(std::vector<gcap_device_info_t> &list) = 0;

    /**
     * @brief Opens the specified device (based on the index returned by enumerate()).
     * @param index Device index from the enumerated list.
     * @return true if the device was opened successfully.
     */
    virtual bool open(int index) = 0;

    /**
     * @brief Set the capture profile (resolution, frame rate, pixel format).
     * @param p Capture profile parameters.
     * @return true if the profile was accepted and applied.
     */
    virtual bool setProfile(const gcap_profile_t &p) = 0;

    /**
     * @brief Allocate or configure frame buffers.
     * @param count Number of buffers.
     * @param bytes_hint Recommended buffer size in bytes.
     * @return true if buffers were allocated successfully.
     */
    virtual bool setBuffers(int count, size_t bytes_hint) = 0;

    /**
     * @brief Start video streaming.
     * @return true if streaming started successfully.
     */
    virtual bool start() = 0;

    /**
     * @brief Stop video streaming.
     */
    virtual void stop() = 0;

    /**
     * @brief Close the currently opened capture device.
     */
    virtual void close() = 0;

    /**
     * @brief Register video and error callbacks.
     * @param vcb Video frame callback.
     * @param ecb Error callback.
     * @param user User pointer passed to callbacks.
     */
    virtual void setCallbacks(gcap_on_video_cb vcb, gcap_on_error_cb ecb, void *user) = 0;

    // --- OBS-like properties ---
    virtual bool getDeviceProps(gcap_device_props_t &out)
    {
        (void)out;
        return false;
    }
    virtual bool getSignalStatus(gcap_signal_status_t &out)
    {
        (void)out;
        return false;
    }
    virtual bool setProcessing(const gcap_processing_opts_t &opts)
    {
        (void)opts;
        return false;
    }
};

/**
 * @brief High-level capture manager.
 *
 * This class manages the selected capture provider, unifies
 * function calls, handles callbacks, and returns standardized
 * status codes for the API.
 */
class CaptureManager
{
public:
    CaptureManager();
    ~CaptureManager();

    gcap_status_t enumerate(gcap_device_info_t *out, int max, int *count);
    gcap_status_t open(int deviceIndex);
    gcap_status_t setProfile(const gcap_profile_t &p);
    gcap_status_t setBuffers(int count, size_t bytes_hint);
    gcap_status_t setCallbacks(gcap_on_video_cb v, gcap_on_error_cb e, void *user);
    gcap_status_t start();
    gcap_status_t startRecording(const char *pathUtf8);
    gcap_status_t stopRecording();
    gcap_status_t setRecordingAudioDevice(const char *deviceIdUtf8);
    gcap_status_t stop();
    gcap_status_t close();
    gcap_status_t getDeviceProps(gcap_device_props_t &out);
    gcap_status_t getSignalStatus(gcap_signal_status_t &out);
    gcap_status_t setProcessing(const gcap_processing_opts_t &opts);

    static void setBackendInt(int v);
    static void setD3dAdapterInt(int index);

private:
    std::unique_ptr<ICaptureProvider> provider_; // Active provider instance
    gcap_on_video_cb vcb_ = nullptr;             // Video frame callback
    gcap_on_error_cb ecb_ = nullptr;             // Error callback
    void *user_ = nullptr;                       // User data pointer for callbacks
};
