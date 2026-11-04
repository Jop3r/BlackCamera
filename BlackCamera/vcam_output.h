#pragma once
// Picks and drives the virtual camera backend: the built-in Windows 11 camera
// (MfVirtualCamOutput) or the OBS Virtual Camera (ObsVirtualCamOutput).
//
// Auto prefers the Windows camera and falls back to OBS when it is not
// available (Windows 10, DLL not registered) or fails to register.

#include "mf_vcam.h"
#include "obs_vcam.h"
#include "vcam_status.h"

#include <opencv2/core.hpp>

#include <chrono>

class VirtualCameraOutput {
public:
    // Call once per frame before send(): applies the backend choice and retries failed starts
    void update(VcamBackend wanted, int width, int height, double fps);
    void send(const cv::Mat& bgr);
    void stop();

    // Backend in use (or being retried); Windows or Obs
    VcamBackend activeBackend() const { return active_; }
    VcamStatus status() const { return status_; }
    // Auto mode had to give up on the Windows camera
    bool fellBack() const { return wanted_ == VcamBackend::Auto && windowsFailed_; }

private:
    using Clock = std::chrono::steady_clock;

    void setStatus(VcamBackend backend, VcamStatus st);
    VcamStatus startWindows();

    VcamBackend wanted_ = VcamBackend::Auto;
    VcamBackend active_ = VcamBackend::Windows;
    VcamStatus  status_ = VcamStatus::Off;
    bool initialized_ = false;
    bool windowsFailed_ = false; // Auto: stop trying the Windows camera for this run

    MfVirtualCamOutput  windows_;
    ObsVirtualCamOutput obs_;
    Clock::time_point lastWindowsAttempt_{}, lastObsAttempt_{};
};
