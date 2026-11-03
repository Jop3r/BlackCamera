#include "face_tracker.h"
#include "log.h"
#include "platform.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <vector>

namespace {

const cv::Size kDetectSize(320, 240);
constexpr int    kDetectEvery = 3;           // run YuNet on every 3rd frame to save CPU
constexpr size_t kOccupancyHistory = 15;     // median filter window
constexpr float  kHudSmoothing = 0.12f;      // EMA factor for box/landmarks
constexpr float  kFaceLostHoldSec = 1.5f;    // keep the zoom this long after losing the face

} // namespace

bool FaceTracker::load(const std::filesystem::path& modelPath) {
    try {
        detector_ = cv::FaceDetectorYN::create(platform::narrow(modelPath.wstring()), "", kDetectSize, 0.8f, 0.3f, 5000);
        LOG_INFO("Loaded YuNet face tracking model");
        return true;
    } catch (const cv::Exception& e) {
        LOG_WARN("Failed to load YuNet model (%s), face zoom disabled: %s",
                 platform::narrow(modelPath.wstring()).c_str(), e.what());
        detector_.reset();
        return false;
    }
}

void FaceTracker::detect(const cv::Mat& frame, const Settings& s, int outW, int outH) {
    cv::Mat small;
    cv::resize(frame, small, kDetectSize);
    detector_->setInputSize(kDetectSize);
    cv::Mat faces;
    detector_->detect(small, faces);

    // YuNet row: box[0..3], landmarks[4..13] (right eye, left eye, nose, right mouth, left mouth), score[14]
    int best = -1;
    float maxArea = 0.0f;
    for (int i = 0; i < faces.rows; ++i) {
        if (faces.at<float>(i, 14) < 0.55f) continue;
        float area = faces.at<float>(i, 2) * faces.at<float>(i, 3);
        if (area > maxArea) { maxArea = area; best = i; }
    }

    if (best < 0) {
        std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() - lastFaceTime_;
        if (elapsed.count() < kFaceLostHoldSec) {
            targetFont_ = lastDetectedFont_;
        } else {
            // Slowly decay back to the minimum font size
            targetFont_ += (s.minFont - targetFont_) * 0.03f;
            hud_.visible = false;
        }
        return;
    }

    const float* f = faces.ptr<float>(best);
    const float sx = (float)outW / kDetectSize.width;
    const float sy = (float)outH / kDetectSize.height;
    auto pt = [&](int col) { return cv::Point2f(f[col] * sx, f[col + 1] * sy); };
    const cv::Rect2f box(f[0] * sx, f[1] * sy, f[2] * sx, f[3] * sy);

    if (!hud_.visible) {
        hud_.box = box;
        hud_.rightEye = pt(4);   hud_.leftEye = pt(6);   hud_.nose = pt(8);
        hud_.rightMouth = pt(10); hud_.leftMouth = pt(12);
        hud_.visible = true;
    } else {
        auto ema = [](auto& cur, const auto& raw) { cur += (raw - cur) * kHudSmoothing; };
        cv::Point2f tl = hud_.box.tl(), size(hud_.box.width, hud_.box.height);
        ema(tl, box.tl());
        ema(size, cv::Point2f(box.width, box.height));
        hud_.box = cv::Rect2f(tl.x, tl.y, size.x, size.y);
        ema(hud_.rightEye, pt(4));
        ema(hud_.leftEye, pt(6));
        ema(hud_.nose, pt(8));
        ema(hud_.rightMouth, pt(10));
        ema(hud_.leftMouth, pt(12));
    }
    hud_.confidence = f[14];

    // Median-filtered face occupancy of the detector frame
    const float occ = (f[2] * f[3]) / (float)kDetectSize.area();
    occupancyHistory_.push_back(occ);
    if (occupancyHistory_.size() > kOccupancyHistory) occupancyHistory_.pop_front();
    std::vector<float> sorted(occupancyHistory_.begin(), occupancyHistory_.end());
    std::sort(sorted.begin(), sorted.end());
    const float median = sorted[sorted.size() / 2];

    // Hysteresis (dead zone) against zoom jitter
    if (std::abs(median - activeOccupancy_) > s.hysteresis * activeOccupancy_ || activeOccupancy_ == 0.0f)
        activeOccupancy_ = median;

    float zoom = 0.0f;
    if (activeOccupancy_ > s.minOccupancy && s.maxOccupancy > s.minOccupancy)
        zoom = std::clamp((activeOccupancy_ - s.minOccupancy) / (s.maxOccupancy - s.minOccupancy), 0.0f, 1.0f);

    targetFont_ = s.minFont + zoom * (s.maxFont - s.minFont);
    lastDetectedFont_ = targetFont_;
    lastFaceTime_ = std::chrono::steady_clock::now();
}

float FaceTracker::update(const cv::Mat& frame, bool cameraLive, const Settings& s, int outW, int outH) {
    if (!initialized_) {
        currentFont_ = targetFont_ = smoothedTargetFont_ = lastDetectedFont_ = s.minFont;
        lastFaceTime_ = std::chrono::steady_clock::now();
        initialized_ = true;
    }

    const bool zoomOn = cameraLive && s.faceZoom && loaded();
    if (zoomOn) {
        // Frames between detections keep the previous target and HUD state
        if (++frameCounter_ % kDetectEvery == 0) detect(frame, s, outW, outH);
    } else {
        targetFont_ = s.minFont;
        hud_.visible = false;
    }

    // Double-layer jitter filtering: a fast EMA over detection jumps, then the user-defined zoom speed
    smoothedTargetFont_ += (targetFont_ - smoothedTargetFont_) * 0.08f;
    currentFont_ += (smoothedTargetFont_ - currentFont_) * s.zoomSpeed;
    return currentFont_;
}
