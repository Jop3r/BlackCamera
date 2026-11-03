#pragma once
// ASCII renderer: glyph atlas + CPU compositing.
//
// DirectWrite rasterizes every character of the charset once into a coverage
// mask (the atlas). Each frame is then assembled by stamping those masks into
// a BGR image. The per-frame cost does not depend on text layout (which was
// ~200k glyphs at small font sizes), and the result is already on the CPU for
// the OBS output and the preview. The atlas is rebuilt only when the integer
// font size, the charset or the antialiasing mode changes.

#include "face_tracker.h"
#include "settings.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <opencv2/core.hpp>

#include <string>
#include <vector>

class AsciiRenderer {
public:
    // The calling thread must have COM initialized (WIC)
    bool init(int width, int height);

    // Converts a camera frame into an ASCII image of the configured size, with the HUD on top
    cv::Mat render(const cv::Mat& bgrFrame, float fontSize, const Settings& s,
                   const std::wstring& charset, const FaceHud& hud);

private:
    bool buildAtlas(int fontPx, const std::wstring& chars, int aaMode);
    void stampGlyph(cv::Mat& out, const cv::Mat& glyph, int ox, int oy, const cv::Vec3b& c) const;
    void drawHud(cv::Mat& out, const FaceHud& hud, const Settings& s, float fontSize, int cols, int rows) const;

    Microsoft::WRL::ComPtr<ID2D1Factory>       d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory>     dwriteFactory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;

    int width_ = 0, height_ = 0;

    // Atlas and the parameters it was built for
    int atlasFontPx_ = -1;
    int atlasAA_ = -1;
    std::wstring atlasChars_;
    int cellW_ = 0, cellH_ = 0;
    std::vector<cv::Mat> glyphs_; // CV_8UC1 coverage masks, cellH x cellW
};
