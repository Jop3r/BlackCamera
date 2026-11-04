#include "pipeline.h"
#include "ascii_renderer.h"
#include "camera.h"
#include "face_tracker.h"
#include "log.h"
#include "platform.h"
#include "vcam_output.h"

#include <windows.h>
#include <objbase.h>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kOutputFps = 30.0;
constexpr int    kMaxFailedReads = 15;                         // then the camera is treated as gone
constexpr auto   kReopenInterval = std::chrono::seconds(1);    // retry while offline
constexpr auto   kPreferredCheckInterval = std::chrono::seconds(3); // look for the preferred camera while on a fallback

} // namespace

Pipeline::Pipeline(SettingsStore& settings, int outWidth, int outHeight)
    : settings_(settings), outW_(outWidth), outH_(outHeight) {}

void Pipeline::start() {
    if (running_) return;
    running_ = true;
    alive_ = true;
    thread_ = std::thread(&Pipeline::run, this);
}

void Pipeline::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

bool Pipeline::takePreview(cv::Mat& rgb) {
    std::lock_guard lock(mutex_);
    if (!previewFresh_) return false;
    rgb = preview_;
    previewFresh_ = false;
    return true;
}

std::string Pipeline::activeCamera() const {
    std::lock_guard lock(mutex_);
    return activeCamera_;
}

void Pipeline::setActiveCamera(const std::string& name) {
    std::lock_guard lock(mutex_);
    activeCamera_ = name;
}

void Pipeline::run() {
    const bool comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    FaceTracker tracker;
    tracker.load(platform::exeDir() / L"face_detection_yunet_2023mar.onnx");

    AsciiRenderer renderer;
    if (!renderer.init(outW_, outH_)) {
        LOG_ERROR("Failed to init ASCII renderer (Direct2D/DirectWrite/WIC)");
        alive_ = false;
        if (comInitialized) CoUninitialize();
        return;
    }

    VirtualCameraOutput vcam;

    CameraSource camera;
    std::string requestedCamera;   // settings value the camera was opened for
    Clock::time_point lastOpenAttempt{}, lastPreferredCheck{};
    cv::Mat lastGoodFrame;
    int failedReads = 0;
    int offlineAnim = 0;

    int frames = 0;
    auto fpsWindowStart = Clock::now();

    while (running_) {
        const Settings s = settings_.get();
        const auto now = Clock::now();

        // ── Camera selection ────────────────────────────────────────────────
        if (camera.isOpen() && s.cameraName != requestedCamera) {
            camera.close(); // user picked another camera
            setActiveCamera({});
        }
        if (camera.isOpen() && !s.cameraName.empty() && camera.name() != s.cameraName &&
            now - lastPreferredCheck > kPreferredCheckInterval) {
            // Running on a fallback: switch back once the preferred camera shows up again
            lastPreferredCheck = now;
            const auto devices = enumerateCameras();
            if (std::any_of(devices.begin(), devices.end(), [&](const CameraDevice& d) { return d.name == s.cameraName; })) {
                camera.close();
                setActiveCamera({});
            }
        }
        if (!camera.isOpen() && now - lastOpenAttempt > kReopenInterval) {
            lastOpenAttempt = now;
            requestedCamera = s.cameraName;
            if (camera.open(s.cameraName, outW_, outH_)) {
                setActiveCamera(camera.name());
                failedReads = 0;
            }
        }

        // ── Next input frame ────────────────────────────────────────────────
        cv::Mat frame;
        bool live = false;
        if (camera.isOpen()) {
            switch (camera.read(frame)) {
                case CameraSource::ReadResult::Skipped:
                    continue;
                case CameraSource::ReadResult::Frame:
                    live = true;
                    failedReads = 0;
                    frame.copyTo(lastGoodFrame);
                    break;
                case CameraSource::ReadResult::Failed:
                    if (++failedReads >= kMaxFailedReads) {
                        LOG_WARN("Camera \"%s\" stopped delivering frames", camera.name().c_str());
                        camera.close();
                        setActiveCamera({});
                        failedReads = 0;
                    } else if (!lastGoodFrame.empty()) {
                        frame = lastGoodFrame.clone();
                        live = true;
                    }
                    break;
            }
        }
        if (frame.empty()) {
            drawOfflineScreen(frame, outW_, outH_, offlineAnim++);
            std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 FPS offline animation
        }

        // ── Render ──────────────────────────────────────────────────────────
        const float fontSize = tracker.update(frame, live, s, outW_, outH_);
        const cv::Mat ascii = renderer.render(frame, fontSize, s, s.charset(), tracker.hud());

        // ── Outputs ─────────────────────────────────────────────────────────
        vcam.update((VcamBackend)s.vcamBackend, outW_, outH_, kOutputFps);
        vcam.send(ascii);
        vcamStatus_ = (int)vcam.status();
        vcamBackend_ = (int)vcam.activeBackend();
        vcamFellBack_ = vcam.fellBack();

        cv::Mat rgb;
        cv::cvtColor(ascii, rgb, cv::COLOR_BGR2RGB);
        {
            std::lock_guard lock(mutex_);
            preview_ = rgb;
            previewFresh_ = true;
        }

        ++frames;
        std::chrono::duration<float> window = Clock::now() - fpsWindowStart;
        if (window.count() >= 1.0f) {
            workerFps_ = frames / window.count();
            frames = 0;
            fpsWindowStart = Clock::now();
        }
    }

    camera.close();
    vcam.stop();
    vcamStatus_ = (int)VcamStatus::Off;
    setActiveCamera({});
    if (comInitialized) CoUninitialize();
    alive_ = false;
}
