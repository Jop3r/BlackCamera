/*
 * BlackCamera — Real-time ASCII camera effect
 * ── Direct2D + Asynchronous Pipeline Upgrade ─────────────────────────────────
 */

#pragma comment(linker, "/SUBSYSTEM:console")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <opencv2/opencv.hpp>
#include <opencv2/objdetect.hpp>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <GL/gl.h>

// GL_CLAMP_TO_EDGE may not be defined in Windows' old GL/gl.h
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────────────────
// GUI Settings & Shared State
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<int>   OUT_W(1920); // Upgraded default resolution for D2D!
static std::atomic<int>   OUT_H(1080);
static bool               comInitialized = false;
static std::atomic<float> MIN_FONT_SZ(10.0f);
static std::atomic<float> MAX_FONT_SZ(45.0f);
static std::atomic<float> GUI_MaxOccupancy(0.15f); 
static int   TARGET_FPS    = 60;
static std::atomic<int>   CAM_INDEX(0);
static std::atomic<bool>  g_cameraActive(false);

static std::atomic<int>   GUI_Brightness(0);      // Additive offset [-100, 100]
static std::atomic<float> GUI_Contrast(1.0f);     // Multiplier [0.1, 3.0]
static std::atomic<int>   GUI_ColorMode(2); // 0 = Custom Monochrome, 1 = Camera Color, 2 = Matrix Green, 3 = Amber CRT, 4 = Cyberpunk Gradient, 5 = Rainbow Gradient
static std::atomic<int>   GUI_AntialiasMode(3); // 0 = Default, 1 = ClearType, 2 = Grayscale, 3 = Aliased (Retro Sharp)
static std::atomic<bool>  GUI_EnableFaceZoom(true);
static char               GUI_CustomCharSet[256] = " .:-=+*#%@";
static std::atomic<float> GUI_FaceZoomSpeed(0.2f);
static std::atomic<int>   GUI_MinBrightnessThreshold(30);
static std::atomic<int>   GUI_BlurSize(3);
static std::atomic<bool>  GUI_ShowCyberHUD(true);
static std::atomic<float> GUI_FaceHysteresis(0.06f);
static std::atomic<float> GUI_MinOccupancy(0.015f);

static std::mutex g_stateMutex;
static float GUI_Color[3] = {0.0f, 1.0f, 0.2f};

static cv::Mat g_latestRGBFrame;
static bool    g_newFrameReady = false;
static std::atomic<bool> g_appRunning(true);
static std::atomic<float> g_workerFPS(0.0f);

static int GUI_CharSetPreset = 1; // Standard preset by default
static std::wstring g_charSet = L" .:-=+*#%@";
static std::atomic<int> g_lastColorMode(2);  // For brush cache invalidation

// ─────────────────────────────────────────────────────────────────────────────
// Virtual Camera OBS Struct (Preserved exactly as before)
// ─────────────────────────────────────────────────────────────────────────────
#define SHARED_QUEUE_STATE_STARTING 1
#define SHARED_QUEUE_STATE_READY    2
#define SHARED_QUEUE_STATE_STOPPING 3

struct queue_header {
	volatile uint32_t write_idx;
	volatile uint32_t read_idx;
	volatile uint32_t state;
	uint32_t offsets[3];
	uint32_t type;
	uint32_t cx;
	uint32_t cy;
	uint64_t interval;
	uint32_t reserved[8];
};

struct ObsVcamSlot {
    HANDLE hMap   = nullptr;
    HANDLE hEvent = nullptr;
    void*  pMem   = nullptr;
    queue_header* header = nullptr;
    uint64_t* ts[3] = {};
    uint8_t* frame[3] = {};
    int w = 0, h = 0;

    bool open(int outWidth, int outHeight) {
        w = outWidth;
        h = outHeight;
        DWORD frame_size = w * h * 3 / 2; // NV12
        DWORD size = sizeof(queue_header);
        size = (size + 31) & ~31;
        uint32_t offset_frame[3];
        for (int i=0; i<3; ++i) {
            offset_frame[i] = size;
            size += frame_size + 32;
            size = (size + 31) & ~31;
        }

        hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, L"OBSVirtualCamVideo");
        if (!hMap) return false;

        hEvent = CreateEventW(NULL, FALSE, FALSE, L"OBSVirtualCamEvent");
        pMem = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!pMem) { CloseHandle(hMap); return false; }
        
        header = (queue_header*)pMem;
        memset(header, 0, sizeof(queue_header));
        header->state = SHARED_QUEUE_STATE_STARTING;
        header->cx = w;
        header->cy = h;
        header->interval = 333333; // 30 FPS default
        header->type = 0;
        
        for (int i=0; i<3; ++i) {
            header->offsets[i] = offset_frame[i];
            ts[i] = (uint64_t*)((uint8_t*)header + offset_frame[i]);
            frame[i] = (uint8_t*)header + offset_frame[i] + 32;
        }
        return true;
    }

    void writeFrame(const cv::Mat& bgr) {
        if (!header) return;
        cv::Mat src = bgr;
        if (src.cols != w || src.rows != h) cv::resize(src, src, cv::Size(w, h));
            
        cv::Mat i420;
        cv::cvtColor(src, i420, cv::COLOR_BGR2YUV_I420);
        long inc = ++header->write_idx;
        int idx = inc % 3;
        
        static LARGE_INTEGER freq = {0};
        if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER current_time;
        QueryPerformanceCounter(&current_time);
        double time_val = (double)current_time.QuadPart;
        time_val *= 1000000000.0;
        time_val /= (double)freq.QuadPart;
        *ts[idx] = static_cast<uint64_t>(time_val);
        
        uint8_t* dstY = frame[idx];
        uint8_t* dstUV = frame[idx] + w * h;
        const uint8_t* srcY = i420.ptr();
        const uint8_t* srcU = srcY + w * h;
        const uint8_t* srcV = srcU + w * h / 4;
        
        memcpy(dstY, srcY, w * h);
        int uvLen = w * h / 4;
        for (int i = 0; i < uvLen; ++i) {
            dstUV[i * 2 + 0] = srcU[i];
            dstUV[i * 2 + 1] = srcV[i];
        }
        header->read_idx = inc;
        header->state = SHARED_QUEUE_STATE_READY;
        if (hEvent) SetEvent(hEvent);
    }

    void close() {
        if (header) header->state = SHARED_QUEUE_STATE_STOPPING;
        if (pMem)  { UnmapViewOfFile(pMem); pMem = nullptr; header = nullptr; }
        if (hMap)  { CloseHandle(hMap);     hMap = nullptr; }
        if (hEvent) { CloseHandle(hEvent); hEvent = nullptr; }
    }
    ~ObsVcamSlot() { close(); }
};

static std::string exeDir() {
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    auto p = s.find_last_of("\\/");
    return (p != std::string::npos) ? s.substr(0, p + 1) : "./";
}

// ─────────────────────────────────────────────────────────────────────────────
// Direct2D Hardware Renderer
// ─────────────────────────────────────────────────────────────────────────────
class D2DRenderer {
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<IDWriteFactory> dwriteFactory;

    ComPtr<ID3D11Texture2D> renderTexture;
    ComPtr<ID3D11Texture2D> stagingTexture;
    ComPtr<ID2D1Bitmap1> targetBitmap;
    
    ComPtr<IDWriteTextFormat> textFormat;
    ComPtr<IDWriteTextFormat> hudTextFormat;
    ComPtr<ID2D1SolidColorBrush> globalBrush;
    
    int width = 0, height = 0;
    float currentFontSize = -1.0f;

    ComPtr<ID2D1SolidColorBrush> brushCache[512];

    ID2D1SolidColorBrush* getCachedBrush(float r, float g, float b) {
        int ir = std::clamp((int)(r * 7.99f), 0, 7);
        int ig = std::clamp((int)(g * 7.99f), 0, 7);
        int ib = std::clamp((int)(b * 7.99f), 0, 7);
        int idx = (ir << 6) | (ig << 3) | ib;
        if (!brushCache[idx]) {
            d2dContext->CreateSolidColorBrush(D2D1::ColorF(ir/7.0f, ig/7.0f, ib/7.0f, 1.0f), &brushCache[idx]);
        }
        return brushCache[idx].Get();
    }

    ID2D1SolidColorBrush* getCachedBrush(uchar r, uchar g, uchar b) {
        int ir = r >> 5;
        int ig = g >> 5;
        int ib = b >> 5;
        int idx = (ir << 6) | (ig << 3) | ib;
        if (!brushCache[idx]) {
            d2dContext->CreateSolidColorBrush(D2D1::ColorF(ir/7.0f, ig/7.0f, ib/7.0f, 1.0f), &brushCache[idx]);
        }
        return brushCache[idx].Get();
    }

    void clearBrushCache() {
        for (int i = 0; i < 512; ++i) {
            brushCache[i].Reset();
        }
    }

    void notifyColorModeChange(int newColorMode) {
        if (newColorMode != g_lastColorMode.exchange(newColorMode)) {
            clearBrushCache();
        }
    }

public:
    bool init(int w, int h) {
        width = w; height = h;
        
        for (int i = 0; i < 512; ++i) {
            brushCache[i].Reset();
        }
        
        // 1. D3D11 Device
        UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, 0, creationFlags, 
                                       featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, 
                                       &d3dDevice, nullptr, &d3dContext);
        if (FAILED(hr)) return false;

        // 2. DXGI & D2D Device
        ComPtr<IDXGIDevice> dxgiDevice;
        d3dDevice.As(&dxgiDevice);

        D2D1_FACTORY_OPTIONS options{};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, (void**)&d2dFactory);
        d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
        d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext);
        
        // 3. DirectWrite
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dwriteFactory);
        dwriteFactory->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"en-US", &hudTextFormat
        );
        
        // 4. Textures
        D3D11_TEXTURE2D_DESC texDesc{};
        texDesc.Width = w;
        texDesc.Height = h;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d3dDevice->CreateTexture2D(&texDesc, nullptr, &renderTexture);
        
        texDesc.Usage = D3D11_USAGE_STAGING;
        texDesc.BindFlags = 0;
        texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d3dDevice->CreateTexture2D(&texDesc, nullptr, &stagingTexture);
        
        // 5. D2D Target Bitmap
        ComPtr<IDXGISurface> dxgiSurface;
        renderTexture.As(&dxgiSurface);
        
        D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
            
        d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &bitmapProperties, &targetBitmap);
        d2dContext->SetTarget(targetBitmap.Get());
        d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED); // Sharp ASCII
        
        d2dContext->CreateSolidColorBrush(D2D1::ColorF(1,1,1), &globalBrush);
        return true;
    }

    void setFontSize(float size) {
        if (std::abs(size - currentFontSize) < 0.1f) return;
        currentFontSize = size;
        textFormat.Reset();
        dwriteFactory->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, L"en-US", &textFormat
        );
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    cv::Mat render(const cv::Mat& bgrFrame, float fontSize, float contrast, float brightness,
                  const float* color_rgb, int colorMode, int antialiasMode, const std::wstring& charSet,
                  int threshold, bool faceDetected = false, float fx = 0.f, float fy = 0.f,
                  float fw = 0.f, float fh = 0.f, float fConf = 0.f,
                  float leX = 0.f, float leY = 0.f, float reX = 0.f, float reY = 0.f,
                  float nX = 0.f, float nY = 0.f, float lmx = 0.f, float lmy = 0.f,
                  float rmx = 0.f, float rmy = 0.f) {
        notifyColorModeChange(colorMode);
        setFontSize(fontSize);

        int fs = std::max(2, (int)fontSize);
        int charW = std::max(1, fs / 2);
        int cols = width / charW;
        int rows = height / fs;

        if (cols <= 0 || rows <= 0) return cv::Mat::zeros(height, width, CV_8UC3);

        cv::Mat gray, resizedGray, resizedColor;
        cv::cvtColor(bgrFrame, gray, cv::COLOR_BGR2GRAY);

        int blurSz = GUI_BlurSize.load();
        if (blurSz > 1) {
            if (blurSz % 2 == 0) blurSz += 1;
            cv::GaussianBlur(gray, gray, cv::Size(blurSz, blurSz), 0);
        }

        gray.convertTo(gray, CV_8U, contrast, brightness);
        cv::resize(gray, resizedGray, cv::Size(cols, rows), 0, 0, cv::INTER_LINEAR);

        if (colorMode == 1) {
            cv::resize(bgrFrame, resizedColor, cv::Size(cols, rows), 0, 0, cv::INTER_LINEAR);
        }

        // Set text antialiasing mode dynamically
        switch (antialiasMode) {
            case 0: d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_DEFAULT); break;
            case 1: d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE); break;
            case 2: d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); break;
            case 3: d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED); break;
        }

        d2dContext->BeginDraw();
        d2dContext->Clear(D2D1::ColorF(0,0,0,1));

        if (colorMode == 0) {
            globalBrush->SetColor(D2D1::ColorF(color_rgb[0], color_rgb[1], color_rgb[2], 1.0f));
        } else if (colorMode == 2) { // Matrix Green
            globalBrush->SetColor(D2D1::ColorF(0.0f, 1.0f, 0.2f, 1.0f));
        } else if (colorMode == 3) { // Amber CRT
            globalBrush->SetColor(D2D1::ColorF(1.0f, 0.65f, 0.0f, 1.0f));
        }

        int nch = (int)charSet.size();

        // Fast path: monochrome modes - single DrawTextW call
        if (colorMode == 0 || colorMode == 2 || colorMode == 3) {
            std::wstring screenText;
            screenText.reserve((cols + 1) * rows);
            for (int y = 0; y < rows; ++y) {
                const uchar* row = resizedGray.ptr<uchar>(y);
                for (int x = 0; x < cols; ++x) {
                    uchar px = row[x];
                    if (px < threshold) {
                        screenText.push_back(L' ');
                    } else {
                        int idx = std::clamp((int)((px / 256.0f) * nch), 0, nch - 1);
                        screenText.push_back(charSet[idx]);
                    }
                }
                screenText.push_back(L'\n');
            }
            D2D1_RECT_F rect = D2D1::RectF(0.0f, 0.0f, (float)width, (float)height);
            d2dContext->DrawTextW(screenText.c_str(), (uint32_t)screenText.length(), textFormat.Get(), rect, globalBrush.Get());
        } else {
            // Cached TextLayout for colored modes - reuse if grid size unchanged
            static int cachedCols = -1, cachedRows = -1;
            static float cachedFontSize = -1;
            static ComPtr<IDWriteTextLayout> cachedTextLayout;
            static std::wstring cachedScreenText;

            bool layoutChanged = (cols != cachedCols || rows != cachedRows || std::abs(fontSize - cachedFontSize) > 0.1f);

            if (layoutChanged) {
                std::wstring screenText;
                screenText.reserve((cols + 1) * rows);
                for (int y = 0; y < rows; ++y) {
                    const uchar* row = resizedGray.ptr<uchar>(y);
                    for (int x = 0; x < cols; ++x) {
                        uchar px = row[x];
                        if (px < threshold) {
                            screenText.push_back(L' ');
                        } else {
                            int idx = std::clamp((int)((px / 256.0f) * nch), 0, nch - 1);
                            screenText.push_back(charSet[idx]);
                        }
                    }
                    screenText.push_back(L'\n');
                }

                HRESULT hr = dwriteFactory->CreateTextLayout(
                    screenText.c_str(), (uint32_t)screenText.length(),
                    textFormat.Get(), (float)width, (float)height, &cachedTextLayout
                );

                if (SUCCEEDED(hr)) {
                    cachedScreenText = std::move(screenText);
                    cachedCols = cols;
                    cachedRows = rows;
                    cachedFontSize = fontSize;
                }
            }

            if (cachedTextLayout) {
                // Apply per-character colors via DrawingEffect
                if (colorMode == 1) { // Camera Color
                    for (int y = 0; y < rows; ++y) {
                        const cv::Vec3b* colorRow = resizedColor.ptr<cv::Vec3b>(y);
                        for (int x = 0; x < cols; ++x) {
                            uint32_t charIdx = y * (cols + 1) + x;
                            if (cachedScreenText[charIdx] == L' ') continue;
                            cv::Vec3b c = colorRow[x];
                            ID2D1SolidColorBrush* brush = getCachedBrush(c[2], c[1], c[0]);
                            DWRITE_TEXT_RANGE range = { charIdx, 1 };
                            cachedTextLayout->SetDrawingEffect(brush, range);
                        }
                    }
                } else if (colorMode == 4) { // Cyberpunk Gradient
                    for (int y = 0; y < rows; ++y) {
                        for (int x = 0; x < cols; ++x) {
                            uint32_t charIdx = y * (cols + 1) + x;
                            if (cachedScreenText[charIdx] == L' ') continue;
                            float t = (float)x / std::max(1, cols - 1);
                            ID2D1SolidColorBrush* brush = getCachedBrush(t, 1.0f - t, 1.0f - t * 0.2f);
                            DWRITE_TEXT_RANGE range = { charIdx, 1 };
                            cachedTextLayout->SetDrawingEffect(brush, range);
                        }
                    }
                } else if (colorMode == 5) { // Rainbow Gradient
                    for (int y = 0; y < rows; ++y) {
                        for (int x = 0; x < cols; ++x) {
                            uint32_t charIdx = y * (cols + 1) + x;
                            if (cachedScreenText[charIdx] == L' ') continue;
                            float t = (float)x / std::max(1, cols - 1) * 3.0f + (float)y / std::max(1, rows - 1) * 2.0f;
                            float r = sin(t) * 0.5f + 0.5f;
                            float g = sin(t + 2.094f) * 0.5f + 0.5f;
                            float b = sin(t + 4.188f) * 0.5f + 0.5f;
                            ID2D1SolidColorBrush* brush = getCachedBrush(r, g, b);
                            DWRITE_TEXT_RANGE range = { charIdx, 1 };
                            cachedTextLayout->SetDrawingEffect(brush, range);
                        }
                    }
                }
                d2dContext->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), cachedTextLayout.Get(), globalBrush.Get());
            }
        }

        // ─────────────────────────────────────────────────────────────────────────────
        // Cyber HUD Overlay Rendering
        // ─────────────────────────────────────────────────────────────────────────────
        if (faceDetected && GUI_ShowCyberHUD.load()) {
            ComPtr<ID2D1SolidColorBrush> hudBrush;
            D2D1_COLOR_F hudColor = D2D1::ColorF(0.0f, 1.0f, 0.2f, 0.8f); // Matrix Green default
            if (colorMode == 0) { // Custom
                hudColor = D2D1::ColorF(color_rgb[0], color_rgb[1], color_rgb[2], 0.8f);
            } else if (colorMode == 3) { // Amber CRT
                hudColor = D2D1::ColorF(1.0f, 0.65f, 0.0f, 0.8f);
            } else if (colorMode == 4) { // Cyberpunk Pink
                hudColor = D2D1::ColorF(1.0f, 0.0f, 0.5f, 0.8f);
            } else if (colorMode == 5) { // Cyan
                hudColor = D2D1::ColorF(0.0f, 0.8f, 1.0f, 0.8f);
            } else if (colorMode == 1) { // BGR Camera Color
                hudColor = D2D1::ColorF(0.0f, 1.0f, 0.8f, 0.8f);
            }
            d2dContext->CreateSolidColorBrush(hudColor, &hudBrush);

            // Draw Face Bounding Box Brackets
            float x = fx;
            float y = fy;
            float w = fw;
            float h = fh;
            float L = std::min(w, h) * 0.25f; // Bracket length (25% of size)
            if (L < 10.0f) L = 10.0f;

            // Draw corner brackets using d2dContext->DrawLine
            // Top-Left corner
            d2dContext->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x + L, y), hudBrush.Get(), 2.5f);
            d2dContext->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x, y + L), hudBrush.Get(), 2.5f);

            // Top-Right corner
            d2dContext->DrawLine(D2D1::Point2F(x + w, y), D2D1::Point2F(x + w - L, y), hudBrush.Get(), 2.5f);
            d2dContext->DrawLine(D2D1::Point2F(x + w, y), D2D1::Point2F(x + w, y + L), hudBrush.Get(), 2.5f);

            // Bottom-Left corner
            d2dContext->DrawLine(D2D1::Point2F(x, y + h), D2D1::Point2F(x + L, y + h), hudBrush.Get(), 2.5f);
            d2dContext->DrawLine(D2D1::Point2F(x, y + h), D2D1::Point2F(x, y + h - L), hudBrush.Get(), 2.5f);

            // Bottom-Right corner
            d2dContext->DrawLine(D2D1::Point2F(x + w, y + h), D2D1::Point2F(x + w - L, y + h), hudBrush.Get(), 2.5f);
            d2dContext->DrawLine(D2D1::Point2F(x + w, y + h), D2D1::Point2F(x + w, y + h - L), hudBrush.Get(), 2.5f);

            // Draw center reticle
            float cx = x + w * 0.5f;
            float cy = y + h * 0.5f;
            d2dContext->DrawLine(D2D1::Point2F(cx - 8.0f, cy), D2D1::Point2F(cx + 8.0f, cy), hudBrush.Get(), 1.5f);
            d2dContext->DrawLine(D2D1::Point2F(cx, cy - 8.0f), D2D1::Point2F(cx, cy + 8.0f), hudBrush.Get(), 1.5f);
            d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 4.0f, 4.0f), hudBrush.Get(), 1.5f);

            // Draw Eye/Nose/Mouth Reticles
            auto drawCross = [&](float px, float py, float size) {
                d2dContext->DrawLine(D2D1::Point2F(px - size, py), D2D1::Point2F(px + size, py), hudBrush.Get(), 1.2f);
                d2dContext->DrawLine(D2D1::Point2F(px, py - size), D2D1::Point2F(px, py + size), hudBrush.Get(), 1.2f);
            };
            drawCross(leX, leY, 5.0f);
            drawCross(reX, reY, 5.0f);

            d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(nX, nY), 3.0f, 3.0f), hudBrush.Get(), 1.5f);
            d2dContext->DrawLine(D2D1::Point2F(lmx, lmy), D2D1::Point2F(rmx, rmy), hudBrush.Get(), 1.5f);

            // Cyber hud text status next to box with futuristic line connector
            float lineStartX = x + w;
            float lineStartY = y + 20.0f;
            float lineMidX = lineStartX + 25.0f;
            float lineMidY = lineStartY - 25.0f;
            float lineEndX = lineMidX + 110.0f;

            d2dContext->DrawLine(D2D1::Point2F(lineStartX, lineStartY), D2D1::Point2F(lineMidX, lineMidY), hudBrush.Get(), 1.5f);
            d2dContext->DrawLine(D2D1::Point2F(lineMidX, lineMidY), D2D1::Point2F(lineEndX, lineMidY), hudBrush.Get(), 1.5f);

            wchar_t infoBuf[256];
            swprintf_s(infoBuf, 256,
                L"SYSTEM: LOCK ACTIVE\n"
                L"TARGET: HUMAN FACE\n"
                L"CONFID: %d%%\n"
                L"FONT  : %.1f px\n"
                L"GRID  : %d x %d",
                (int)(fConf * 100.0f), fontSize, cols, rows
            );

            D2D1_RECT_F textRect = D2D1::RectF(lineMidX + 5.0f, lineMidY - 80.0f, lineMidX + 300.0f, lineMidY - 2.0f);
            d2dContext->DrawTextW(infoBuf, (uint32_t)wcslen(infoBuf), hudTextFormat.Get(), textRect, hudBrush.Get());
        }

        d2dContext->EndDraw();

        // Copy to staging and read to CPU - async readback via double-buffered staging textures
        static int stagingIndex = 0;
        static ComPtr<ID3D11Texture2D> stagingTextures[2];

        // Initialize staging textures on first call
        if (!stagingTextures[0]) {
            D3D11_TEXTURE2D_DESC texDesc{};
            texDesc.Width = width;
            texDesc.Height = height;
            texDesc.MipLevels = 1;
            texDesc.ArraySize = 1;
            texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_STAGING;
            texDesc.BindFlags = 0;
            texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            d3dDevice->CreateTexture2D(&texDesc, nullptr, &stagingTextures[0]);
            d3dDevice->CreateTexture2D(&texDesc, nullptr, &stagingTextures[1]);
        }

        // Copy to current staging texture (async GPU copy)
        d3dContext->CopyResource(stagingTextures[stagingIndex].Get(), renderTexture.Get());

        // Map PREVIOUS frame's staging texture (one frame latency, but no stall)
        int readIndex = 1 - stagingIndex;
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = d3dContext->Map(stagingTextures[readIndex].Get(), 0, D3D11_MAP_READ, 0, &mapped);

        cv::Mat result(height, width, CV_8UC3);
        if (SUCCEEDED(hr)) {
            const uint8_t* src = (const uint8_t*)mapped.pData;
            int pitch = mapped.RowPitch;

            // Fast memcpy per row using OpenCV ParallelFor
            cv::parallel_for_(cv::Range(0, height), [&](const cv::Range& range) {
                for (int r = range.start; r < range.end; ++r) {
                    const uint8_t* s = src + r * pitch;
                    uint8_t* d = result.ptr<uint8_t>(r);
                    for (int c = 0; c < width; ++c) {
                        d[c*3 + 0] = s[c*4 + 0]; // B
                        d[c*3 + 1] = s[c*4 + 1]; // G
                        d[c*3 + 2] = s[c*4 + 2]; // R
                    }
                }
            });
            d3dContext->Unmap(stagingTextures[readIndex].Get(), 0);
        } else {
            // First frame or map failed - return black
            result = cv::Mat::zeros(height, width, CV_8UC3);
        }

        stagingIndex = readIndex; // swap for next frame
        return result;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Asynchronous Worker Thread
// ─────────────────────────────────────────────────────────────────────────────
void CaptureAndRenderThread() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) comInitialized = true;

    cv::VideoCapture cap;
    cv::Ptr<cv::FaceDetectorYN> faceDetector;
    try {
        std::string modelPath = exeDir() + "face_detection_yunet_2023mar.onnx";
        faceDetector = cv::FaceDetectorYN::create(modelPath, "", cv::Size(320, 240), 0.8f, 0.3f, 5000);
        std::cout << "[INFO] Loaded YuNet Face Tracking model.\n";
    } catch (...) {
        std::cout << "[WARN] Failed to load YuNet ONNX model! Fallback to static font size.\n";
    }

    D2DRenderer renderer;
    if (!renderer.init(OUT_W.load(), OUT_H.load())) {
        std::cerr << "[ERROR] Failed to init Direct2D Renderer!" << std::endl;
        g_appRunning = false;
        if (comInitialized) CoUninitialize();
        return;
    }
    
    ObsVcamSlot obsVcam;
    bool useObs = obsVcam.open(OUT_W.load(), OUT_H.load());
    if (useObs) std::cout << "[INFO] Streaming to OBS Virtual Camera at " << OUT_W.load() << "x" << OUT_H.load() << ".\n";

    cv::Mat lastGoodFrame;
    int failedReadCount = 0;
    float currentFontSize = MIN_FONT_SZ.load();
    float targetFontSize = currentFontSize;
    float smoothedTargetFontSize = currentFontSize;
    float lastDetectedFontSize = currentFontSize;
    auto lastFaceTime = std::chrono::steady_clock::now();
    auto lastTime = std::chrono::steady_clock::now();
    int frames = 0;
    int fallbackFrameCount = 0;
    int lastOpenedIndex = -1;

    float activeFaceOcc = 0.0f;
    std::deque<float> occHistory;
    constexpr size_t OCC_HISTORY_MAX = 15;
    
    // Smooth HUD face variables
    bool  hudFaceDetected = false;
    float hudFaceX = 0.0f, hudFaceY = 0.0f, hudFaceW = 0.0f, hudFaceH = 0.0f;
    float hudConfidence = 0.0f;
    float hudLEyeX = 0.0f, hudLEyeY = 0.0f;
    float hudREyeX = 0.0f, hudREyeY = 0.0f;
    float hudNoseX = 0.0f, hudNoseY = 0.0f;
    float hudLMouthX = 0.0f, hudLMouthY = 0.0f;
    float hudRMouthX = 0.0f, hudRMouthY = 0.0f;

    while (g_appRunning) {
        cv::Mat frame;
        bool hasFrame = false;
        int targetCamIdx = CAM_INDEX.load();

        // Frame rate limiter: target ~30 FPS for worker thread
        static auto lastFrameTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<float> frameTime = now - lastFrameTime;
        float targetFrameTime = 1.0f / 30.0f; // 30 FPS cap
        if (frameTime.count() < targetFrameTime) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        lastFrameTime = now;

        if (g_cameraActive.load()) {
            if (targetCamIdx != lastOpenedIndex) {
                cap.release();
                g_cameraActive = false;
                failedReadCount = 0;
            } else {
                cv::Mat tempFrame;
                if (cap.read(tempFrame) && !tempFrame.empty()) {
                    frame = tempFrame;
                    hasFrame = true;
                    cv::flip(frame, frame, 1);
                    frame.copyTo(lastGoodFrame);
                    failedReadCount = 0;
                } else {
                    failedReadCount++;
                    if (failedReadCount >= 15) { // Tolerate up to 15 dropped frames (~0.25 sec @ 60fps) before going offline
                        cap.release();
                        g_cameraActive = false;
                        failedReadCount = 0;
                    } else if (!lastGoodFrame.empty()) {
                        frame = lastGoodFrame.clone();
                        hasFrame = true;
                    }
                }
            }
        }

        if (!g_cameraActive.load()) {
            if (cap.open(targetCamIdx)) {
                cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
                cap.set(cv::CAP_PROP_FPS, TARGET_FPS);
                cap.set(cv::CAP_PROP_FRAME_WIDTH, OUT_W.load());
                cap.set(cv::CAP_PROP_FRAME_HEIGHT, OUT_H.load());
                int cap_w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
                int cap_h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
                if (cap_w > 0 && cap_h > 0) {
                    OUT_W.store(cap_w);
                    OUT_H.store(cap_h);
                }
                lastOpenedIndex = targetCamIdx;
                g_cameraActive = true;
                std::cout << "[INFO] Camera " << targetCamIdx << " opened successfully.\n";
            } else {
                // If requested camera fails to open, look for any online webcam device (0 to 3)
                bool autoFound = false;
                for (int i = 0; i < 4; ++i) {
                    if (i == targetCamIdx) continue;
                    if (cap.open(i)) {
                        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
                        cap.set(cv::CAP_PROP_FPS, TARGET_FPS);
                        cap.set(cv::CAP_PROP_FRAME_WIDTH, OUT_W.load());
                        cap.set(cv::CAP_PROP_FRAME_HEIGHT, OUT_H.load());
                        CAM_INDEX.store(i);
                        lastOpenedIndex = i;
                        g_cameraActive = true;
                        autoFound = true;
                        std::cout << "[INFO] Camera " << targetCamIdx << " unavailable. Auto-switched to active Camera index " << i << ".\n";
                        break;
                    }
                }

                if (!autoFound) {
                    // Generate an ultra-premium retro sci-fi animated scanning screen when camera is offline
                    frame = cv::Mat::zeros(OUT_H.load(), OUT_W.load(), CV_8UC3);
                    cv::Point center(OUT_W / 2, OUT_H / 2);
                    int maxR = std::min(OUT_W.load(), OUT_H.load()) / 3;
                    
                    // Render tech grid
                    int gridSpacing = 40;
                    int gridShift = (fallbackFrameCount * 2) % gridSpacing;
                    for (int x = gridShift; x < OUT_W.load(); x += gridSpacing) {
                        cv::line(frame, cv::Point(x, 0), cv::Point(x, OUT_H), cv::Scalar(0, 40, 0), 1);
                    }
                    for (int y = gridShift; y < OUT_H.load(); y += gridSpacing) {
                        cv::line(frame, cv::Point(0, y), cv::Point(OUT_W, y), cv::Scalar(0, 40, 0), 1);
                    }

                    // Rotating radar sweep line
                    double angle = (fallbackFrameCount * 3) * CV_PI / 180.0;
                    cv::Point sweepPoint(center.x + maxR * cos(angle), center.y + maxR * sin(angle));
                    cv::line(frame, center, sweepPoint, cv::Scalar(0, 255, 0), 3);
                    cv::circle(frame, center, 10, cv::Scalar(0, 255, 0), -1);

                    // Pulsing green scanning rings
                    for (int r = 50; r < maxR; r += 100) {
                        int pulseR = (r + fallbackFrameCount * 3) % maxR;
                        int alpha = 255 - (pulseR * 255 / maxR);
                        cv::circle(frame, center, pulseR, cv::Scalar(0, alpha, 0), 2);
                    }

                    // Cyberpunk alert text
                    cv::putText(frame, "VIDEO SIGNAL OFFLINE", cv::Point(center.x - 300, center.y - 40), cv::FONT_HERSHEY_SIMPLEX, 1.6, cv::Scalar(0, 255, 255), 3);
                    cv::putText(frame, "SCANNING INPUT DEVICE INDEX [0 - 3]...", cv::Point(center.x - 320, center.y + 40), cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(0, 200, 200), 2);
                    
                    hasFrame = true;
                    fallbackFrameCount++;
                    std::this_thread::sleep_for(std::chrono::milliseconds(33)); // Target ~30fps for offline render animation
                }
            }
        }

        if (hasFrame) {
            // Face detection at reduced frequency (every 3 frames) to save CPU
            static int faceDetectCounter = 0;
            bool runFaceDetect = g_cameraActive.load() && GUI_EnableFaceZoom.load() && faceDetector && (++faceDetectCounter % 3 == 0);

            if (runFaceDetect) {
                cv::Mat smallFrame;
                cv::resize(frame, smallFrame, cv::Size(320, 240));
                faceDetector->setInputSize(cv::Size(320, 240));

                cv::Mat faces;
                faceDetector->detect(smallFrame, faces);

                int bestFaceIdx = -1;
                float maxArea = 0.0f;
                for (int i = 0; i < faces.rows; ++i) {
                    float confidence = faces.at<float>(i, 4);
                    if (confidence >= 0.55f) {
                        float w = faces.at<float>(i, 2);
                        float h = faces.at<float>(i, 3);
                        float area = w * h;
                        if (area > maxArea) {
                            maxArea = area;
                            bestFaceIdx = i;
                        }
                    }
                }

                if (bestFaceIdx != -1) {
                    float x = faces.at<float>(bestFaceIdx, 0);
                    float y = faces.at<float>(bestFaceIdx, 1);
                    float w = faces.at<float>(bestFaceIdx, 2);
                    float h = faces.at<float>(bestFaceIdx, 3);
                    float conf = faces.at<float>(bestFaceIdx, 4);
                    
                    float scaleX = (float)OUT_W.load() / 320.0f;
                    float scaleY = (float)OUT_H.load() / 240.0f;
                    
                    float rawX = x * scaleX;
                    float rawY = y * scaleY;
                    float rawW = w * scaleX;
                    float rawH = h * scaleY;
                    
                    // Smooth visual box coordinates using EMA
                    if (!hudFaceDetected) {
                        hudFaceX = rawX;
                        hudFaceY = rawY;
                        hudFaceW = rawW;
                        hudFaceH = rawH;
                        
                        hudLEyeX = faces.at<float>(bestFaceIdx, 5) * scaleX;
                        hudLEyeY = faces.at<float>(bestFaceIdx, 6) * scaleY;
                        hudREyeX = faces.at<float>(bestFaceIdx, 7) * scaleX;
                        hudREyeY = faces.at<float>(bestFaceIdx, 8) * scaleY;
                        hudNoseX = faces.at<float>(bestFaceIdx, 9) * scaleX;
                        hudNoseY = faces.at<float>(bestFaceIdx, 10) * scaleY;
                        hudLMouthX = faces.at<float>(bestFaceIdx, 11) * scaleX;
                        hudLMouthY = faces.at<float>(bestFaceIdx, 12) * scaleY;
                        hudRMouthX = faces.at<float>(bestFaceIdx, 13) * scaleX;
                        hudRMouthY = faces.at<float>(bestFaceIdx, 14) * scaleY;
                        
                        hudFaceDetected = true;
                    } else {
                        float f = 0.12f; // Smooth tracking movement
                        hudFaceX += (rawX - hudFaceX) * f;
                        hudFaceY += (rawY - hudFaceY) * f;
                        hudFaceW += (rawW - hudFaceW) * f;
                        hudFaceH += (rawH - hudFaceH) * f;
                        
                        hudLEyeX += (faces.at<float>(bestFaceIdx, 5) * scaleX - hudLEyeX) * f;
                        hudLEyeY += (faces.at<float>(bestFaceIdx, 6) * scaleY - hudLEyeY) * f;
                        hudREyeX += (faces.at<float>(bestFaceIdx, 7) * scaleX - hudREyeX) * f;
                        hudREyeY += (faces.at<float>(bestFaceIdx, 8) * scaleY - hudREyeY) * f;
                        hudNoseX += (faces.at<float>(bestFaceIdx, 9) * scaleX - hudNoseX) * f;
                        hudNoseY += (faces.at<float>(bestFaceIdx, 10) * scaleY - hudNoseY) * f;
                        hudLMouthX += (faces.at<float>(bestFaceIdx, 11) * scaleX - hudLMouthX) * f;
                        hudLMouthY += (faces.at<float>(bestFaceIdx, 12) * scaleY - hudLMouthY) * f;
                        hudRMouthX += (faces.at<float>(bestFaceIdx, 13) * scaleX - hudRMouthX) * f;
                        hudRMouthY += (faces.at<float>(bestFaceIdx, 14) * scaleY - hudRMouthY) * f;
                    }
                    hudConfidence = conf;
                    
                    float occ = (w * h) / (320.0f * 240.0f);
                    
                    // Push to occupancy history for median filtering
                    occHistory.push_back(occ);
                    if (occHistory.size() > OCC_HISTORY_MAX) {
                        occHistory.pop_front();
                    }
                    
                    // Calculate median occupancy
                    std::vector<float> sortedOcc(occHistory.begin(), occHistory.end());
                    std::sort(sortedOcc.begin(), sortedOcc.end());
                    float medianOcc = sortedOcc[sortedOcc.size() / 2];
                    
                    // Apply Hysteresis (Dead Zone) to occupancy
                    float hyst = GUI_FaceHysteresis.load();
                    if (std::abs(medianOcc - activeFaceOcc) > hyst * activeFaceOcc || activeFaceOcc == 0.0f) {
                        activeFaceOcc = medianOcc;
                    }
                    
                    // Map stabilized occupancy to font size
                    float maxOcc = GUI_MaxOccupancy.load();
                    float minOcc = GUI_MinOccupancy.load();
                    float zoomRatio = 0.0f;
                    if (activeFaceOcc > minOcc) {
                        zoomRatio = std::min((activeFaceOcc - minOcc) / (maxOcc - minOcc), 1.0f);
                        zoomRatio = std::max(0.0f, zoomRatio);
                    }
                    
                    targetFontSize = MIN_FONT_SZ.load() + zoomRatio * (MAX_FONT_SZ.load() - MIN_FONT_SZ.load());
                    lastDetectedFontSize = targetFontSize;
                    lastFaceTime = std::chrono::steady_clock::now();
                } else {
                    auto now = std::chrono::steady_clock::now();
                    std::chrono::duration<float> elapsed = now - lastFaceTime;
                    if (elapsed.count() < 1.5f) {
                        targetFontSize = lastDetectedFontSize;
                    } else {
                        // Slowly decay back to minimum font size
                        targetFontSize += (MIN_FONT_SZ.load() - targetFontSize) * 0.03f;
                        hudFaceDetected = false;
                    }
                }
            } else {
                targetFontSize = MIN_FONT_SZ.load();
                hudFaceDetected = false;
            }
            
            // Double-layer jitter filtering: first filter face detection jumps with a fast EMA
            smoothedTargetFontSize = smoothedTargetFontSize + (targetFontSize - smoothedTargetFontSize) * 0.08f;
            
            // Then smoothly interpolate currentFontSize towards the filtered size at user-defined speed
            currentFontSize += (smoothedTargetFontSize - currentFontSize) * GUI_FaceZoomSpeed.load();
            
            // Read UI state
            float brightness = GUI_Brightness.load();
            float contrast = GUI_Contrast.load();
            int colorMode = GUI_ColorMode.load();
            int antialiasMode = GUI_AntialiasMode.load();
            int threshold = GUI_MinBrightnessThreshold.load();
            std::wstring charSet;
            float color[3];
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                color[0] = GUI_Color[0]; color[1] = GUI_Color[1]; color[2] = GUI_Color[2];
                charSet = g_charSet;
            }

            // Hardware Render (using correct swapped brightness/contrast params and passing Cyber HUD params)
            cv::Mat ascii = renderer.render(
                frame, currentFontSize, contrast, brightness, color, colorMode, antialiasMode, charSet, threshold,
                hudFaceDetected, hudFaceX, hudFaceY, hudFaceW, hudFaceH, hudConfidence,
                hudLEyeX, hudLEyeY, hudREyeX, hudREyeY, hudNoseX, hudNoseY,
                hudLMouthX, hudLMouthY, hudRMouthX, hudRMouthY
            );
            
            // Output to OBS
            if (useObs) obsVcam.writeFrame(ascii);

            // Send to GUI
            cv::Mat rgb;
            cv::cvtColor(ascii, rgb, cv::COLOR_BGR2RGB);
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_latestRGBFrame = rgb;
                g_newFrameReady = true;
            }

            // FPS Counter
            frames++;
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<float> diff = now - lastTime;
            if (diff.count() >= 1.0f) {
                g_workerFPS = (float)frames / diff.count();
                frames = 0;
                lastTime = now;
            }
        }
    }
    
    cap.release();
    obsVcam.close();
    if (comInitialized) CoUninitialize();
}

// Convert UTF-8 std::string to std::wstring
static std::wstring Utf8ToWstring(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main UI Thread
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    if (!glfwInit()) {
        MessageBoxA(NULL, "Failed to initialize GLFW.", "BlackCamera Error", MB_ICONERROR);
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1024, 768, "BlackCamera - ImGui Dashboard (Direct2D Accelerated)", NULL, NULL);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // OpenGL Texture for preview
    GLuint previewTex = 0;

    glGenTextures(1, &previewTex);
    glBindTexture(GL_TEXTURE_2D, previewTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Keep preview dimensions persistent outside loop to prevent flickering
    int frame_w = 0;
    int frame_h = 0;

    // Start Worker Thread
    std::thread worker(CaptureAndRenderThread);

    // Main loop
    while (!glfwWindowShouldClose(window) && g_appRunning.load()) {
        glfwPollEvents();

        // Update OpenGL Texture from latest async frame
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            if (g_newFrameReady && !g_latestRGBFrame.empty()) {
                glBindTexture(GL_TEXTURE_2D, previewTex);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, g_latestRGBFrame.cols, g_latestRGBFrame.rows, 0, GL_RGB, GL_UNSIGNED_BYTE, g_latestRGBFrame.ptr());
                g_newFrameReady = false;
                frame_w = g_latestRGBFrame.cols;
                frame_h = g_latestRGBFrame.rows;
            }
        }
        
        // Render GUI
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);

        // Fullscreen Preview Window (Behind GUI)
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)display_w, (float)display_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Preview", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs);
        if (frame_w > 0 && frame_h > 0) {
            float scale = std::min((float)display_w / frame_w, (float)display_h / frame_h);
            ImVec2 size(frame_w * scale, frame_h * scale);
            ImVec2 pos((display_w - size.x) * 0.5f, (display_h - size.y) * 0.5f);
            ImGui::SetCursorPos(pos);
            ImGui::Image((ImTextureID)(intptr_t)previewTex, size);
        }
        ImGui::End();
        ImGui::PopStyleVar();

        // Control Panel
        ImGui::Begin("BlackCamera Control Panel", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        
        if (ImGui::Button("Restore Default Settings")) {
            GUI_Brightness.store(0);
            GUI_Contrast.store(1.0f);
            GUI_ColorMode.store(2); // Matrix Green
            GUI_MinBrightnessThreshold.store(30);
            GUI_EnableFaceZoom.store(true);
            MIN_FONT_SZ.store(10.0f);
            MAX_FONT_SZ.store(45.0f);
            GUI_FaceZoomSpeed.store(0.2f);
            GUI_CharSetPreset = 1; // Standard Preset
            GUI_AntialiasMode.store(3); // Aliased
            GUI_ShowCyberHUD.store(true);
            GUI_FaceHysteresis.store(0.06f);
            GUI_MinOccupancy.store(0.015f);
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_charSet = L" .:-=+*#%@";
                GUI_Color[0] = 0.0f; GUI_Color[1] = 1.0f; GUI_Color[2] = 0.2f;
            }
        }
        ImGui::Separator();
        
        ImGui::Text("Hardware Accelerated Rendering (Direct2D)");
        ImGui::Separator();
        
        ImGui::Text("Webcam Device Input");
        int camIdx = CAM_INDEX.load();
        if (ImGui::SliderInt("Webcam ID", &camIdx, 0, 3)) {
            CAM_INDEX.store(camIdx);
        }
        if (g_cameraActive.load()) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Webcam Status: ACTIVE (Index %d)", CAM_INDEX.load());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Webcam Status: OFFLINE / SCANNING...");
        }
        ImGui::Separator();

        int bright = GUI_Brightness.load();
        if (ImGui::SliderInt("Brightness", &bright, -100, 100)) GUI_Brightness.store(bright);

        float contrast = GUI_Contrast.load();
        if (ImGui::SliderFloat("Contrast", &contrast, 0.1f, 3.0f)) GUI_Contrast.store(contrast);

        ImGui::Separator();;
        ImGui::Text("Text Rendering Options");
        
        const char* aaModes[] = { "Default", "ClearType (Smooth)", "Grayscale (Anti-aliased)", "Aliased (Retro Sharp)" };
        int aaMode = GUI_AntialiasMode.load();
        if (ImGui::Combo("Antialiasing", &aaMode, aaModes, IM_ARRAYSIZE(aaModes))) {
            GUI_AntialiasMode.store(aaMode);
        }

        ImGui::Separator();
        ImGui::Text("Blur / Preprocessing");
        
        int blurSz = GUI_BlurSize.load();
        if (ImGui::SliderInt("Blur Kernel Size", &blurSz, 1, 31)) {
            if (blurSz % 2 == 0) blurSz += 1;  // Must be odd
            GUI_BlurSize.store(blurSz);
        }

        ImGui::Separator();
        ImGui::Text("Color Options & Themes");
        
        const char* colorModes[] = { "Custom Monochrome", "Camera Color", "Matrix Green", "Amber CRT (Retro)", "Cyberpunk Gradient", "Rainbow Wave" };
        int colMode = GUI_ColorMode.load();
        if (ImGui::Combo("Color Mode", &colMode, colorModes, IM_ARRAYSIZE(colorModes))) {
            GUI_ColorMode.store(colMode);
        }
        
        if (colMode == 0) {
            float tempColor[3];
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                tempColor[0] = GUI_Color[0]; tempColor[1] = GUI_Color[1]; tempColor[2] = GUI_Color[2];
            }
            if (ImGui::ColorEdit3("ASCII Color", tempColor)) {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                GUI_Color[0] = tempColor[0]; GUI_Color[1] = tempColor[1]; GUI_Color[2] = tempColor[2];
            }
        }

        ImGui::Separator();
        ImGui::Text("ASCII Presets & Density");
        
        const char* presets[] = { "Sparse", "Standard", "Classic Unicode", "Binary (0/1)", "Matrix Rain", "Custom..." };
        if (ImGui::Combo("Character Preset", &GUI_CharSetPreset, presets, IM_ARRAYSIZE(presets))) {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            switch (GUI_CharSetPreset) {
                case 0: g_charSet = L"      .:-=+*#%@"; break;
                case 1: g_charSet = L" .:-=+*#%@"; break;
                case 2: g_charSet = L" .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$"; break;
                case 3: g_charSet = L" 01"; break;
                case 4: g_charSet = L" ｦｱｳｴｵｶｷｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝ1234567890:*+-<>|"; break;
                case 5: {
                    if (strlen(GUI_CustomCharSet) == 0) {
                        strcpy_s(GUI_CustomCharSet, " .:+=#@");
                    }
                    g_charSet = Utf8ToWstring(GUI_CustomCharSet);
                    break;
                }
            }
        }
        
        if (GUI_CharSetPreset == 5) {
            if (ImGui::InputText("Custom Palette", GUI_CustomCharSet, IM_ARRAYSIZE(GUI_CustomCharSet))) {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_charSet = Utf8ToWstring(GUI_CustomCharSet);
            }
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Tip: Order symbols from darkest to brightest.");
        }

        int threshold = GUI_MinBrightnessThreshold.load();
        if (ImGui::SliderInt("Min Brightness Threshold", &threshold, 0, 255)) {
            GUI_MinBrightnessThreshold.store(threshold);
        }

        ImGui::Separator();
        ImGui::Text("Face Tracking & Font Zoom");
        
        bool enableFaceZoom = GUI_EnableFaceZoom.load();
        if (ImGui::Checkbox("Enable Face Tracking Font Zoom", &enableFaceZoom)) {
            GUI_EnableFaceZoom.store(enableFaceZoom);
        }
        
        if (enableFaceZoom) {
            float minF = MIN_FONT_SZ.load();
            if (ImGui::SliderFloat("Min Font Size", &minF, 5.0f, 30.0f)) MIN_FONT_SZ.store(minF);
            
            float maxF = MAX_FONT_SZ.load();
            if (ImGui::SliderFloat("Max Font Size", &maxF, 30.0f, 150.0f)) MAX_FONT_SZ.store(maxF);
            
            float zSpeed = GUI_FaceZoomSpeed.load();
            if (ImGui::SliderFloat("Zoom Speed", &zSpeed, 0.01f, 1.0f)) GUI_FaceZoomSpeed.store(zSpeed);

            ImGui::Separator();
            ImGui::Text("HUD & Advanced Stabilization Settings");
            
            bool showHUD = GUI_ShowCyberHUD.load();
            if (ImGui::Checkbox("Show Cyber HUD Overlay", &showHUD)) {
                GUI_ShowCyberHUD.store(showHUD);
            }
            
            float hyst = GUI_FaceHysteresis.load() * 100.0f;
            if (ImGui::SliderFloat("Zoom Jitter Deadzone (%)", &hyst, 0.0f, 30.0f, "%.1f%%")) {
                GUI_FaceHysteresis.store(hyst / 100.0f);
            }
            
            float maxOcc = GUI_MaxOccupancy.load() * 100.0f;
            if (ImGui::SliderFloat("Max Distance Sensitivity (%)", &maxOcc, 5.0f, 60.0f, "%.1f%%")) {
                GUI_MaxOccupancy.store(maxOcc / 100.0f);
            }

            float minOcc = GUI_MinOccupancy.load() * 100.0f;
            if (ImGui::SliderFloat("Min Zoom Distance Threshold (%)", &minOcc, 0.5f, 20.0f, "%.1f%%")) {
                GUI_MinOccupancy.store(minOcc / 100.0f);
            }
        } else {
            float staticF = MIN_FONT_SZ.load();
            if (ImGui::SliderFloat("Font Size", &staticF, 5.0f, 150.0f)) {
                MIN_FONT_SZ.store(staticF);
            }
        }
        
        ImGui::Separator();
        ImGui::Text("Worker Engine FPS: %.1f", g_workerFPS.load());
        ImGui::Text("GUI Render FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::End();

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    g_appRunning = false;
    worker.join(); // Wait for capture thread to finish cleanly

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
