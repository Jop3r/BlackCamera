#pragma once
// YuNet face detection driving the font zoom and the HUD overlay.

#include "settings.h"

#include <opencv2/core.hpp>
#include <opencv2/objdetect.hpp>

#include <chrono>
#include <deque>
#include <filesystem>

// Smoothed face geometry in output-image pixels
struct FaceHud {
    bool visible = false;
    cv::Rect2f box;
    float confidence = 0.0f;
    cv::Point2f rightEye, leftEye, nose, rightMouth, leftMouth;
};

class FaceTracker {
public:
    bool load(const std::filesystem::path& modelPath);
    bool loaded() const { return !detector_.empty(); }

    // Advances zoom and HUD state by one rendered frame and returns the font size to use.
    // `frame` is the camera image; HUD coordinates are produced for an outW x outH output.
    float update(const cv::Mat& frame, bool cameraLive, const Settings& s, int outW, int outH);
    const FaceHud& hud() const { return hud_; }

private:
    void detect(const cv::Mat& frame, const Settings& s, int outW, int outH);

    cv::Ptr<cv::FaceDetectorYN> detector_;
    FaceHud hud_;

    bool  initialized_ = false;
    int   frameCounter_ = 0;
    float currentFont_ = 0.0f;
    float targetFont_ = 0.0f;
    float smoothedTargetFont_ = 0.0f;
    float lastDetectedFont_ = 0.0f;
    std::chrono::steady_clock::time_point lastFaceTime_;

    float activeOccupancy_ = 0.0f;
    std::deque<float> occupancyHistory_;
};
