#include "camera.h"
#include "log.h"
#include "platform.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kRequestedFps = 60;
constexpr int kOutputFps = 30;

bool isOwnOutputName(const std::string& name) {
    return name.find("OBS Virtual Camera") != std::string::npos;
}

} // namespace

std::vector<CameraDevice> enumerateCameras() {
    std::vector<CameraDevice> list;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return list;

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) &&
        SUCCEEDED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFEnumDeviceSources(attrs.Get(), &devices, &count))) {
            for (UINT32 i = 0; i < count; ++i) {
                CameraDevice dev;
                dev.index = (int)i;
                WCHAR* name = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len))) {
                    dev.name = platform::narrow(name);
                    CoTaskMemFree(name);
                } else {
                    dev.name = "Camera " + std::to_string(i);
                }
                dev.isOwnOutput = isOwnOutputName(dev.name);
                list.push_back(std::move(dev));
                devices[i]->Release();
            }
            CoTaskMemFree(devices);
        }
    }
    MFShutdown();
    return list;
}

bool CameraSource::open(const std::string& preferredName, int width, int height) {
    close();
    const auto devices = enumerateCameras();

    const CameraDevice* chosen = nullptr;
    if (!preferredName.empty()) {
        auto it = std::find_if(devices.begin(), devices.end(),
                               [&](const CameraDevice& d) { return d.name == preferredName; });
        if (it != devices.end()) chosen = &*it;
    }
    if (!chosen) {
        auto it = std::find_if(devices.begin(), devices.end(), [](const CameraDevice& d) { return !d.isOwnOutput; });
        if (it == devices.end()) return false;
        chosen = &*it;
        if (!preferredName.empty())
            LOG_WARN("Camera \"%s\" not found, using \"%s\"", preferredName.c_str(), chosen->name.c_str());
    }

    if (!cap_.open(chosen->index, cv::CAP_MSMF)) {
        LOG_WARN("Failed to open camera \"%s\" (index %d)", chosen->name.c_str(), chosen->index);
        return false;
    }
    cap_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap_.set(cv::CAP_PROP_FPS, kRequestedFps);
    cap_.set(cv::CAP_PROP_FRAME_WIDTH, width);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, height);

    // Render every N-th frame so the output stays at ~30 FPS (N = 2 for a 60 FPS camera)
    const double camFps = cap_.get(cv::CAP_PROP_FPS);
    renderEvery_ = (camFps > 0.0) ? std::max(1, (int)std::lround(camFps / kOutputFps)) : 1;
    frameCounter_ = 0;
    name_ = chosen->name;

    LOG_INFO("Camera \"%s\" opened: %dx%d @ %.0f FPS", name_.c_str(),
             (int)cap_.get(cv::CAP_PROP_FRAME_WIDTH), (int)cap_.get(cv::CAP_PROP_FRAME_HEIGHT), camFps);
    return true;
}

void CameraSource::close() {
    if (cap_.isOpened()) {
        cap_.release();
        LOG_INFO("Camera \"%s\" closed", name_.c_str());
    }
    name_.clear();
}

CameraSource::ReadResult CameraSource::read(cv::Mat& frame) {
    // cap.read() blocks until the next camera frame, so the camera paces the worker loop.
    // A camera faster than the output is still drained (low latency), extra frames are dropped.
    cv::Mat raw;
    if (!cap_.read(raw) || raw.empty()) return ReadResult::Failed;
    if (++frameCounter_ % renderEvery_ != 0) return ReadResult::Skipped;
    cv::flip(raw, frame, 1);
    return ReadResult::Frame;
}

void drawOfflineScreen(cv::Mat& frame, int width, int height, int animFrame) {
    frame = cv::Mat::zeros(height, width, CV_8UC3);
    cv::Point center(width / 2, height / 2);
    int maxR = std::min(width, height) / 3;

    // Tech grid
    int gridSpacing = 40;
    int gridShift = (animFrame * 2) % gridSpacing;
    for (int x = gridShift; x < width; x += gridSpacing)
        cv::line(frame, cv::Point(x, 0), cv::Point(x, height), cv::Scalar(0, 40, 0), 1);
    for (int y = gridShift; y < height; y += gridSpacing)
        cv::line(frame, cv::Point(0, y), cv::Point(width, y), cv::Scalar(0, 40, 0), 1);

    // Rotating radar sweep line
    double angle = (animFrame * 3) * CV_PI / 180.0;
    cv::Point sweepPoint(center.x + (int)(maxR * cos(angle)), center.y + (int)(maxR * sin(angle)));
    cv::line(frame, center, sweepPoint, cv::Scalar(0, 255, 0), 3);
    cv::circle(frame, center, 10, cv::Scalar(0, 255, 0), -1);

    // Pulsing scanning rings
    for (int r = 50; r < maxR; r += 100) {
        int pulseR = (r + animFrame * 3) % maxR;
        int alpha = 255 - (pulseR * 255 / maxR);
        cv::circle(frame, center, pulseR, cv::Scalar(0, alpha, 0), 2);
    }

    cv::putText(frame, "VIDEO SIGNAL OFFLINE", cv::Point(center.x - 300, center.y - 40),
                cv::FONT_HERSHEY_SIMPLEX, 1.6, cv::Scalar(0, 255, 255), 3);
    cv::putText(frame, "WAITING FOR A CAMERA...", cv::Point(center.x - 200, center.y + 40),
                cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(0, 200, 200), 2);
}
