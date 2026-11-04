#include "vcam_output.h"
#include "log.h"

namespace {

constexpr auto kObsRetryInterval = std::chrono::seconds(2);     // OBS may be busy or installed later
constexpr auto kWindowsRetryInterval = std::chrono::seconds(5); // explicit Windows mode only

const char* backendName(VcamBackend b) {
    return b == VcamBackend::Obs ? "OBS Virtual Camera" : "Windows virtual camera";
}

const char* statusText(VcamBackend b, VcamStatus st) {
    switch (st) {
        case VcamStatus::Streaming:    return "streaming";
        case VcamStatus::Waiting:      return "registered as \"BlackCamera\", waiting for an app to open it";
        case VcamStatus::Unsupported:  return "needs Windows 11 (build 22000 or newer)";
        case VcamStatus::NotInstalled: return b == VcamBackend::Obs ? "not installed (install OBS Studio)"
                                                                    : "media source not registered (reinstall BlackCamera)";
        case VcamStatus::Busy:         return "busy (stop the virtual camera in OBS), retrying";
        case VcamStatus::Error:        return b == VcamBackend::Obs ? "failed to open shared memory, retrying"
                                                                    : "registration failed";
        default:                       return "off";
    }
}

} // namespace

void VirtualCameraOutput::setStatus(VcamBackend backend, VcamStatus st) {
    if (backend == active_ && st == status_) return;
    active_ = backend;
    status_ = st;
    if (st == VcamStatus::Streaming || st == VcamStatus::Waiting) LOG_INFO("%s: %s", backendName(backend), statusText(backend, st));
    else LOG_WARN("%s: %s", backendName(backend), statusText(backend, st));
}

VcamStatus VirtualCameraOutput::startWindows() {
    lastWindowsAttempt_ = Clock::now();
    return windows_.start();
}

void VirtualCameraOutput::update(VcamBackend wanted, int width, int height, double fps) {
    if (!initialized_ || wanted != wanted_) {
        stop();
        initialized_ = true;
        wanted_ = wanted;
        windowsFailed_ = false;
        lastWindowsAttempt_ = lastObsAttempt_ = {};
        status_ = VcamStatus::Off;
    }
    const auto now = Clock::now();

    // ── Built-in Windows camera ─────────────────────────────────────────────
    const bool tryWindows = wanted_ == VcamBackend::Windows || (wanted_ == VcamBackend::Auto && !windowsFailed_);
    if (tryWindows) {
        if (!windows_.isOpen() &&
            (lastWindowsAttempt_ == Clock::time_point{} || now - lastWindowsAttempt_ > kWindowsRetryInterval)) {
            const VcamStatus st = startWindows();
            if (!windows_.isOpen()) {
                if (wanted_ == VcamBackend::Auto) {
                    windowsFailed_ = true;
                    LOG_WARN("Windows virtual camera: %s; falling back to OBS Virtual Camera", statusText(VcamBackend::Windows, st));
                } else {
                    setStatus(VcamBackend::Windows, st);
                    return;
                }
            }
        }
        if (windows_.isOpen()) {
            setStatus(VcamBackend::Windows, windows_.status());
            return;
        }
        if (wanted_ == VcamBackend::Windows) return; // keep showing the last error until the next retry
    }

    // ── OBS Virtual Camera ──────────────────────────────────────────────────
    if (!obs_.isOpen() && (lastObsAttempt_ == Clock::time_point{} || now - lastObsAttempt_ > kObsRetryInterval)) {
        lastObsAttempt_ = now;
        setStatus(VcamBackend::Obs, obs_.start(width, height, fps));
    } else if (active_ != VcamBackend::Obs) {
        setStatus(VcamBackend::Obs, obs_.isOpen() ? VcamStatus::Streaming : VcamStatus::Off);
    }
}

void VirtualCameraOutput::send(const cv::Mat& bgr) {
    if (active_ == VcamBackend::Windows) {
        windows_.send(bgr);
        if (windows_.isOpen()) setStatus(VcamBackend::Windows, windows_.status());
    } else {
        obs_.send(bgr);
    }
}

void VirtualCameraOutput::stop() {
    windows_.stop();
    obs_.stop();
    status_ = VcamStatus::Off;
}
