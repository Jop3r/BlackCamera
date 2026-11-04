#pragma once
// Capture -> face tracking -> ASCII render -> virtual camera output, on a worker thread.
// The GUI thread reads status and the latest preview frame from here.

#include "settings.h"
#include "vcam_status.h"

#include <opencv2/core.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class Pipeline {
public:
    Pipeline(SettingsStore& settings, int outWidth, int outHeight);
    ~Pipeline() { stop(); }

    void start();
    void stop();
    // False once the worker has stopped on its own (renderer init failure)
    bool alive() const { return alive_.load(); }

    // Latest rendered frame (RGB) if a new one arrived since the last call
    bool takePreview(cv::Mat& rgb);

    float workerFps() const { return workerFps_.load(); }
    VcamStatus vcamStatus() const { return (VcamStatus)vcamStatus_.load(); }
    // Output in use: Windows (built-in camera) or Obs
    VcamBackend vcamBackend() const { return (VcamBackend)vcamBackend_.load(); }
    // Auto mode is on OBS because the Windows camera was not available
    bool vcamFellBack() const { return vcamFellBack_.load(); }
    // Name of the camera in use; empty while offline
    std::string activeCamera() const;

private:
    void run();
    void setActiveCamera(const std::string& name);

    SettingsStore& settings_;
    const int outW_, outH_;

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> alive_{ false };

    std::atomic<float> workerFps_{ 0.0f };
    std::atomic<int> vcamStatus_{ (int)VcamStatus::Off };
    std::atomic<int> vcamBackend_{ (int)VcamBackend::Windows };
    std::atomic<bool> vcamFellBack_{ false };

    mutable std::mutex mutex_;   // guards everything below
    std::string activeCamera_;
    cv::Mat preview_;
    bool previewFresh_ = false;
};
