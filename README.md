# BlackCamera - C++ Version

Real-time ASCII camera effect with Direct2D hardware acceleration. C++ port of test.py with significant performance improvements.

---

## Dependencies

| Library | Install |
|---------|---------|
| OpenCV 4.x | %USERPROFILE%cpkgcpkg.exe install opencv4:x64-windows |
| GLFW3 | %USERPROFILE%cpkgcpkg.exe install glfw3:x64-windows |
| Dear ImGui | %USERPROFILE%cpkgcpkg.exe install imgui[glfw-binding,opengl3-binding]:x64-windows |
| vcpkg | Already installed at %USERPROFILE%cpkg |

---

## Build

1. Open BlackCamera.slnx in Visual Studio 2022.
2. Select Release | x64 (or Debug | x64).
3. Build -> Build Solution (Ctrl+Shift+B).

Post-build step copies only required OpenCV + GLFW DLLs next to .exe.

---

## Run

BlackCamera.exe

Opens ImGui control panel + fullscreen preview. Close window to exit.

### Virtual Camera (OBS)

1. Start OBS Studio.
2. Tools -> Start Virtual Camera.
3. Run BlackCamera.exe - it auto-detects OBS shared memory and streams there.
4. Select OBS Virtual Camera in any video app.

If OBS is not running, BlackCamera works standalone with preview window.

---

## Settings (top of main.cpp)

| Constant | Default | Description |
|----------|---------|-------------|
| CAM_INDEX | 0 | Webcam device index |
| OUT_W / OUT_H | 1920 / 1080 | Output resolution |
| MIN_FONT_SZ | 10.0f | Minimum font size |
| MAX_FONT_SZ | 45.0f | Maximum font size |
| GUI_MaxOccupancy | 0.15f | Face area fraction for max zoom |
| GUI_MinOccupancy | 0.015f | Face area fraction for min zoom |
| TARGET_FPS | 60 | Target capture FPS |
| GUI_BlurSize | 3 | Gaussian blur kernel (odd) |
| GUI_FaceHysteresis | 0.06f | Zoom dead-zone (6%) |
| GUI_FaceZoomSpeed | 0.2f | Font size interpolation speed |
| GUI_Color | {0,1,0.2} | Custom monochrome color (RGB 0-1) |

### ImGui Runtime Controls

- Brightness (-100..100) - additive offset
- Contrast (0.1..3.0) - multiplicative gain
- Color Mode - Custom / Camera / Matrix Green / Amber CRT / Cyberpunk / Rainbow
- Antialiasing - Default / ClearType / Grayscale / Aliased (retro sharp)
- Character Preset - Sparse / Standard / Classic Unicode / Binary / Matrix Rain / Custom
- Min Brightness Threshold (0..255)
- Face Tracking Font Zoom - enable/disable + Min/Max font, Zoom Speed
- Cyber HUD - toggle face box, landmarks, status overlay
- Advanced Stabilization - Jitter Deadzone, Max Sensitivity, Min Threshold

---

## Why Faster Than Python?

- No GIL - true parallelism
- Direct2D GPU rendering - hardware-accelerated text
- Async pipeline - capture/render on worker thread, UI on main thread
- Persistent resources - D2D bitmap, brushes, text layouts reused
- OpenCV SIMD - GaussianBlur, resize, YuNet use AVX2/SSE
- Frame pacing - worker at camera FPS, UI at display refresh

---

## YuNet Face Detector

Model: face_detection_yunet_2023mar.onnx (included). Detects face box + 5 landmarks + confidence. Input: 320x240.

---

## License

MIT
