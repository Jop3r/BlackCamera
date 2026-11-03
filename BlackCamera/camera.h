#pragma once
// Webcam input: device enumeration by friendly name and frame capture.

#include <opencv2/videoio.hpp>
#include <string>
#include <vector>

struct CameraDevice {
    std::string name;  // friendly name (UTF-8)
    int index = -1;    // cv::CAP_MSMF device index
    bool isOwnOutput = false; // our own OBS output device: never used as an input
};

// Media Foundation video capture devices, in the same order OpenCV's MSMF backend uses.
// The calling thread must have COM initialized.
std::vector<CameraDevice> enumerateCameras();

class CameraSource {
public:
    enum class ReadResult { Frame, Skipped, Failed };

    // Opens `preferredName`, or the first usable camera when it is empty or missing.
    // Requests width x height; the actual camera size may differ.
    bool open(const std::string& preferredName, int width, int height);
    void close();
    bool isOpen() const { return cap_.isOpened(); }
    const std::string& name() const { return name_; }

    // Reads one frame (mirrored). Frames beyond ~30 FPS are reported as Skipped.
    ReadResult read(cv::Mat& frame);

private:
    cv::VideoCapture cap_;
    std::string name_;
    int renderEvery_ = 1;
    int frameCounter_ = 0;
};

// Animated "no signal" screen shown while no camera is available
void drawOfflineScreen(cv::Mat& frame, int width, int height, int animFrame);
