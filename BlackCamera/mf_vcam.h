#pragma once
// Built-in Windows 11 virtual camera output (no OBS needed).
//
// BlackCameraVCam.dll (registered by the installer) is the media source; this
// class registers a session-lifetime, current-user camera named "BlackCamera"
// with MFCreateVirtualCamera and feeds it NV12 frames through the shared memory
// described in vcam_shared.h. The camera disappears when the app exits.

#include "nv12.h"
#include "vcam_status.h"

#include <opencv2/core.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

class MfVirtualCamOutput {
public:
    // Windows 11 (build 22000+) with MFCreateVirtualCamera available
    static bool isSupported();
    // BlackCameraVCam.dll is registered (HKLM, 64-bit view)
    static bool isSourceRegistered();

    // Registers the camera. Returns Waiting on success, an error status otherwise.
    VcamStatus start();
    void send(const cv::Mat& bgr);
    void stop();
    bool isOpen() const { return worker_.joinable(); }
    // Waiting until an app opens the camera, then Streaming
    VcamStatus status() const;

    MfVirtualCamOutput() = default;
    MfVirtualCamOutput(const MfVirtualCamOutput&) = delete;
    MfVirtualCamOutput& operator=(const MfVirtualCamOutput&) = delete;
    ~MfVirtualCamOutput() { stop(); }

private:
    void openMapping();
    void closeMapping();

    std::thread worker_;          // owns the IMFVirtualCamera (MTA) until stop()
    void*       stopEvent_ = nullptr;

    void*    mapping_ = nullptr;  // HANDLE
    uint8_t* base_    = nullptr;
    uint64_t lastOpenAttempt_ = 0;
    std::atomic<bool> consumer_{ false };
    Nv12Converter nv12_;
};
