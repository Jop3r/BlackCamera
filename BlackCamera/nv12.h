#pragma once
// BGR -> NV12 conversion shared by the virtual camera outputs.

#include <opencv2/core.hpp>

#include <cstdint>

class Nv12Converter {
public:
    // Writes a tightly packed NV12 image (Y plane width*height, then interleaved
    // UV plane width*height/2) to dst, scaling bgr to width x height first if needed.
    // width and height must be even.
    void convert(const cv::Mat& bgr, int width, int height, uint8_t* dst);

private:
    cv::Mat scaled_, i420_;
};
