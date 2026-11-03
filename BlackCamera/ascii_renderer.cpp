#include "ascii_renderer.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace {

cv::Vec3b toBGR(float r, float g, float b) {
    return cv::Vec3b(cv::saturate_cast<uchar>(b * 255.0f),
                     cv::saturate_cast<uchar>(g * 255.0f),
                     cv::saturate_cast<uchar>(r * 255.0f));
}

} // namespace

bool AsciiRenderer::init(int width, int height) {
    width_ = width;
    height_ = height;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wicFactory_)))) return false;
    return true;
}

bool AsciiRenderer::buildAtlas(int fontPx, const std::wstring& chars, int aaMode) {
    if (fontPx == atlasFontPx_ && aaMode == atlasAA_ && chars == atlasChars_) return true;
    glyphs_.clear();
    atlasFontPx_ = fontPx;
    atlasAA_ = aaMode;
    atlasChars_ = chars;
    cellH_ = fontPx;
    cellW_ = std::max(1, fontPx / 2);
    if (chars.empty()) return true;

    ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwriteFactory_->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            (float)fontPx, L"en-US", &format))) return false;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Natural glyph box of the monospaced font: one advance wide, one line tall
    ComPtr<IDWriteTextLayout> probe;
    DWRITE_TEXT_METRICS tm{};
    if (FAILED(dwriteFactory_->CreateTextLayout(L"M", 1, format.Get(), 1000.0f, 1000.0f, &probe)) ||
        FAILED(probe->GetMetrics(&tm))) return false;
    const int boxW = std::max(1, (int)std::ceil(tm.widthIncludingTrailingWhitespace));
    const int boxH = std::max(1, (int)std::ceil(tm.height));
    const int n = (int)chars.size();

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wicFactory_->CreateBitmap(boxW * n, boxH, GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapCacheOnLoad, &bitmap))) return false;
    ComPtr<ID2D1RenderTarget> rt;
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(d2dFactory_->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt))) return false;
    ComPtr<ID2D1SolidColorBrush> white;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &white))) return false;

    // ClearType needs an opaque background, so every smooth mode uses grayscale coverage
    rt->SetTextAntialiasMode(aaMode == 3 ? D2D1_TEXT_ANTIALIAS_MODE_ALIASED : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    for (int i = 0; i < n; ++i) {
        // Each glyph is clipped to its own box, so fallback-font glyphs (e.g. katakana) cannot shift the grid
        D2D1_RECT_F box = D2D1::RectF((float)(i * boxW), 0.0f, (float)((i + 1) * boxW), (float)boxH);
        rt->DrawText(&chars[i], 1, format.Get(), box, white.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (FAILED(rt->EndDraw())) return false;

    WICRect rc{ 0, 0, boxW * n, boxH };
    ComPtr<IWICBitmapLock> lock;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, &lock)) ||
        FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) return false;

    // White text on transparent black, premultiplied: alpha is the glyph coverage
    cv::Mat bgra(boxH, boxW * n, CV_8UC4, data, stride);
    cv::Mat coverage;
    cv::extractChannel(bgra, coverage, 3);

    glyphs_.reserve(n);
    for (int i = 0; i < n; ++i) {
        cv::Mat glyph;
        cv::resize(coverage(cv::Rect(i * boxW, 0, boxW, boxH)), glyph, cv::Size(cellW_, cellH_), 0, 0, cv::INTER_AREA);
        if (aaMode == 3) cv::threshold(glyph, glyph, 96, 255, cv::THRESH_BINARY); // keep the retro look crisp
        glyphs_.push_back(glyph);
    }
    return true;
}

// Blends one glyph mask in color c (BGR) onto a black background at (ox, oy)
void AsciiRenderer::stampGlyph(cv::Mat& out, const cv::Mat& glyph, int ox, int oy, const cv::Vec3b& c) const {
    for (int py = 0; py < cellH_; ++py) {
        const uchar* m = glyph.ptr<uchar>(py);
        uchar* o = out.ptr<uchar>(oy + py) + ox * 3;
        for (int px = 0; px < cellW_; ++px, o += 3) {
            unsigned a = m[px];
            if (!a) continue;
            o[0] = (uchar)((c[0] * a + 127) / 255);
            o[1] = (uchar)((c[1] * a + 127) / 255);
            o[2] = (uchar)((c[2] * a + 127) / 255);
        }
    }
}

cv::Mat AsciiRenderer::render(const cv::Mat& bgrFrame, float fontSize, const Settings& s,
                              const std::wstring& charset, const FaceHud& hud) {
    cv::Mat out = cv::Mat::zeros(height_, width_, CV_8UC3);

    int fs = std::max(2, (int)fontSize);
    if (!buildAtlas(fs, charset, s.antialiasMode)) return out;

    int cols = width_ / cellW_;
    int rows = height_ / cellH_;
    if (cols <= 0 || rows <= 0) return out;

    cv::Mat gray, resizedGray, resizedColor;
    cv::cvtColor(bgrFrame, gray, cv::COLOR_BGR2GRAY);
    if (s.blurSize > 1) {
        int k = s.blurSize | 1;
        cv::GaussianBlur(gray, gray, cv::Size(k, k), 0);
    }
    gray.convertTo(gray, CV_8U, s.contrast, s.brightness);
    cv::resize(gray, resizedGray, cv::Size(cols, rows), 0, 0, cv::INTER_LINEAR);

    const int colorMode = s.colorMode;
    if (colorMode == 1) {
        cv::resize(bgrFrame, resizedColor, cv::Size(cols, rows), 0, 0, cv::INTER_LINEAR);
    }

    cv::Vec3b tint = toBGR(1.0f, 1.0f, 1.0f);
    if (colorMode == 0)      tint = toBGR(s.color[0], s.color[1], s.color[2]);
    else if (colorMode == 2) tint = toBGR(0.0f, 1.0f, 0.2f);   // Matrix Green
    else if (colorMode == 3) tint = toBGR(1.0f, 0.65f, 0.0f);  // Amber CRT

    const int nch = (int)glyphs_.size();
    const int threshold = s.minBrightness;
    cv::parallel_for_(cv::Range(0, rows), [&](const cv::Range& range) {
        for (int gy = range.start; gy < range.end; ++gy) {
            const uchar* lum = resizedGray.ptr<uchar>(gy);
            const cv::Vec3b* cam = colorMode == 1 ? resizedColor.ptr<cv::Vec3b>(gy) : nullptr;
            for (int gx = 0; gx < cols; ++gx) {
                uchar px = lum[gx];
                if (px < threshold || nch == 0) continue;
                int idx = std::clamp((int)((px / 256.0f) * nch), 0, nch - 1);
                if (charset[idx] == L' ') continue;

                cv::Vec3b c = tint;
                if (colorMode == 1) { // Camera Color
                    c = cam[gx];
                } else if (colorMode == 4) { // Cyberpunk Gradient
                    float t = (float)gx / std::max(1, cols - 1);
                    c = toBGR(t, 1.0f - t, 1.0f - t * 0.2f);
                } else if (colorMode == 5) { // Rainbow Gradient
                    float t = (float)gx / std::max(1, cols - 1) * 3.0f + (float)gy / std::max(1, rows - 1) * 2.0f;
                    c = toBGR(sin(t) * 0.5f + 0.5f, sin(t + 2.094f) * 0.5f + 0.5f, sin(t + 4.188f) * 0.5f + 0.5f);
                }
                stampGlyph(out, glyphs_[idx], gx * cellW_, gy * cellH_, c);
            }
        }
    });

    if (hud.visible && s.showHud) drawHud(out, hud, s, fontSize, cols, rows);
    return out;
}

void AsciiRenderer::drawHud(cv::Mat& out, const FaceHud& hud, const Settings& s, float fontSize, int cols, int rows) const {
    cv::Vec3b hc = toBGR(0.0f, 1.0f, 0.2f); // Matrix Green default
    if (s.colorMode == 0)      hc = toBGR(s.color[0], s.color[1], s.color[2]); // Custom
    else if (s.colorMode == 3) hc = toBGR(1.0f, 0.65f, 0.0f); // Amber CRT
    else if (s.colorMode == 4) hc = toBGR(1.0f, 0.0f, 0.5f);  // Cyberpunk Pink
    else if (s.colorMode == 5) hc = toBGR(0.0f, 0.8f, 1.0f);  // Cyan
    else if (s.colorMode == 1) hc = toBGR(0.0f, 1.0f, 0.8f);  // Camera Color
    const cv::Scalar col(hc[0], hc[1], hc[2]);
    auto P = [](float x, float y) { return cv::Point(cvRound(x), cvRound(y)); };
    auto line = [&](float x0, float y0, float x1, float y1, int t) {
        cv::line(out, P(x0, y0), P(x1, y1), col, t, cv::LINE_AA);
    };

    // Face bounding box brackets
    const float x = hud.box.x, y = hud.box.y, w = hud.box.width, h = hud.box.height;
    const float L = std::max(10.0f, std::min(w, h) * 0.25f); // bracket length (25% of size)
    line(x, y, x + L, y, 3);                 line(x, y, x, y + L, 3);
    line(x + w, y, x + w - L, y, 3);         line(x + w, y, x + w, y + L, 3);
    line(x, y + h, x + L, y + h, 3);         line(x, y + h, x, y + h - L, 3);
    line(x + w, y + h, x + w - L, y + h, 3); line(x + w, y + h, x + w, y + h - L, 3);

    // Center reticle
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    line(cx - 8.0f, cy, cx + 8.0f, cy, 2);
    line(cx, cy - 8.0f, cx, cy + 8.0f, 2);
    cv::circle(out, P(cx, cy), 4, col, 2, cv::LINE_AA);

    // Eye/nose/mouth reticles
    auto cross = [&](const cv::Point2f& p, float sz) {
        line(p.x - sz, p.y, p.x + sz, p.y, 1);
        line(p.x, p.y - sz, p.x, p.y + sz, 1);
    };
    cross(hud.leftEye, 5.0f);
    cross(hud.rightEye, 5.0f);
    cv::circle(out, P(hud.nose.x, hud.nose.y), 3, col, 2, cv::LINE_AA);
    line(hud.leftMouth.x, hud.leftMouth.y, hud.rightMouth.x, hud.rightMouth.y, 2);

    // Status text next to the box with a connector line
    const float lineStartX = x + w, lineStartY = y + 20.0f;
    const float lineMidX = lineStartX + 25.0f, lineMidY = lineStartY - 25.0f;
    const float lineEndX = lineMidX + 110.0f;
    line(lineStartX, lineStartY, lineMidX, lineMidY, 2);
    line(lineMidX, lineMidY, lineEndX, lineMidY, 2);

    char info[5][48];
    snprintf(info[0], sizeof(info[0]), "SYSTEM: LOCK ACTIVE");
    snprintf(info[1], sizeof(info[1]), "TARGET: HUMAN FACE");
    snprintf(info[2], sizeof(info[2]), "CONFID: %d%%", (int)(hud.confidence * 100.0f));
    snprintf(info[3], sizeof(info[3]), "FONT  : %.1f px", fontSize);
    snprintf(info[4], sizeof(info[4]), "GRID  : %d x %d", cols, rows);
    for (int i = 0; i < 5; ++i) {
        cv::putText(out, info[i], P(lineMidX + 5.0f, lineMidY - 80.0f + 13.0f + i * 16.0f),
                    cv::FONT_HERSHEY_PLAIN, 1.0, col, 1, cv::LINE_AA);
    }
}
