#include "nv12.h"

#include <opencv2/imgproc.hpp>

#include <cstring>

void Nv12Converter::convert(const cv::Mat& bgr, int width, int height, uint8_t* dst) {
    const cv::Mat* src = &bgr;
    if (bgr.cols != width || bgr.rows != height) {
        cv::resize(bgr, scaled_, cv::Size(width, height));
        src = &scaled_;
    }
    cv::cvtColor(*src, i420_, cv::COLOR_BGR2YUV_I420); // planar: Y, U, V

    const size_t lumaBytes = (size_t)width * height;
    std::memcpy(dst, i420_.data, lumaBytes);

    // I420 chroma planes -> NV12 interleaved UV, written straight into dst
    const int cw = width / 2, ch = height / 2;
    const cv::Mat u(ch, cw, CV_8UC1, i420_.data + lumaBytes);
    const cv::Mat v(ch, cw, CV_8UC1, i420_.data + lumaBytes + lumaBytes / 4);
    cv::Mat uv(ch, cw, CV_8UC2, dst + lumaBytes);
    const cv::Mat planes[] = { u, v };
    cv::merge(planes, 2, uv);
}
