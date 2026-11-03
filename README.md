# BlackCamera - C++ Version

Real-time ASCII camera effect with a glyph-atlas renderer (DirectWrite + multithreaded CPU compositing). C++ port of test.py with significant performance improvements.

[🇷🇺 Русская версия](README_RU.md)

[![build](https://github.com/Jop3r/BlackCamera/actions/workflows/build.yml/badge.svg)](https://github.com/Jop3r/BlackCamera/actions/workflows/build.yml)

---

## Build

Requirements: Windows 10/11 x64, Visual Studio 2022 or newer with the **Desktop development with C++** workload, [vcpkg](https://github.com/microsoft/vcpkg). Dependencies (OpenCV, GLFW, Dear ImGui) are declared in `vcpkg.json` and installed automatically.

### CMake (recommended, same as CI)

```bat
set VCPKG_ROOT=%USERPROFILE%\vcpkg
cmake --workflow --preset release
```

The command configures, builds and runs the tests. The executable is in `build\Release\`. The first build compiles OpenCV through vcpkg and can take a while; later builds use the vcpkg binary cache.

Visual Studio can also open the folder directly (**File → Open → Folder**) and use the same preset.

### Visual Studio solution

`BlackCamera.slnx` uses the classic vcpkg layout (`%USERPROFILE%\vcpkg\installed\x64-windows`), so install the packages once:

```bat
%USERPROFILE%\vcpkg\vcpkg.exe install opencv4 glfw3 imgui[glfw-binding,opengl3-binding] --triplet x64-windows
```

Then open `BlackCamera.slnx`, select **Release | x64** and build. The post-build step copies the DLLs and the face model next to the .exe.

### Installer

```bat
powershell -ExecutionPolicy Bypass -File tools\make-installer.ps1
```

Builds Release, stages it in `dist\BlackCamera` and compiles `dist\BlackCamera_Setup_v<version>.exe` with [Inno Setup 6](https://jrsoftware.org/isdl.php). The installer includes the Microsoft Visual C++ Redistributable.

---

## Run

Start `BlackCamera.exe`. It opens the control panel over a fullscreen preview; close the window to exit. There is no console window — see [Logs](#logs).

### Camera

Pick the webcam by name in **Camera**. The choice is remembered by name, so it survives reconnects and changed device order. If the selected camera is missing, BlackCamera uses the first available one and switches back as soon as the selected camera reappears. **OBS Virtual Camera** is never used as an input, because it is BlackCamera's own output.

### Virtual Camera (OBS)

1. Install [OBS Studio](https://obsproject.com/). OBS itself does **not** need to be running.
2. Make sure the virtual camera is **not** started in OBS (**Start Virtual Camera** off) — BlackCamera takes that role itself.
3. Run `BlackCamera.exe`. The control panel shows `OBS Virtual Camera: STREAMING`.
4. Select **OBS Virtual Camera** in any video app (Zoom, Discord, Telegram, browser).

Status in the control panel:

- `BUSY` — the virtual camera is already started in OBS. Stop it there; BlackCamera retries every 2 s.
- `NOT INSTALLED` — OBS Studio is not installed. BlackCamera still works with the preview window.

OBS provides a single virtual camera, so you cannot capture BlackCamera's output in OBS and send it back through the same virtual camera.

---

## Settings

Every control in the panel is saved automatically (about a second after the last change) to `%APPDATA%\BlackCamera\settings.ini`. **Restore Default Settings** resets everything except the selected camera; deleting the file does the same on the next start. The panel layout is stored next to it in `imgui.ini`.

- **Camera** - device by name, or Auto
- **Brightness** (-100..100) - additive offset
- **Contrast** (0.1..3.0) - multiplicative gain
- **Antialiasing** - Default / ClearType / Grayscale / Aliased (retro sharp)
- **Blur Kernel Size** (1..31, odd)
- **Color Mode** - Custom / Camera / Matrix Green / Amber CRT / Cyberpunk / Rainbow
- **Character Preset** - Sparse / Standard / Classic Unicode / Binary / Matrix Rain / Custom
- **Min Brightness Threshold** (0..255)
- **Face Tracking Font Zoom** - enable/disable + Min/Max font, Zoom Speed
- **Cyber HUD** - toggle face box, landmarks, status overlay
- **Advanced Stabilization** - Jitter Deadzone, Max Sensitivity, Min Threshold

Output resolution is fixed at 1920x1080, 30 FPS.

## Logs

`%LOCALAPPDATA%\BlackCamera\blackcamera.log` (the previous run is kept as `blackcamera.prev.log`). It lists the detected cameras, the camera in use, OBS output status and OpenCV warnings. Debug builds also open a console window with the same output.

---

## Source layout

| File | Responsibility |
|------|----------------|
| `main.cpp` | Window, ImGui, preview texture |
| `gui.*` | Control panel, settings auto-save |
| `pipeline.*` | Worker thread: camera → face tracker → renderer → OBS |
| `camera.*` | Device enumeration by name (Media Foundation), capture, offline screen |
| `face_tracker.*` | YuNet detection, font zoom, HUD smoothing |
| `ascii_renderer.*` | Glyph atlas, frame compositing, HUD drawing |
| `obs_vcam.*` | OBS Virtual Camera shared-memory output |
| `settings.*` | Settings struct, `settings.ini` load/save |
| `log.*`, `platform.*` | File logging, Windows helpers |

---

## Why Faster Than Python?

- **No GIL** - true parallelism
- **Glyph atlas** - each character is rasterized once by DirectWrite, frames are assembled by stamping cached glyph masks (no per-frame text layout)
- **Async pipeline** - capture/render on worker thread, UI on main thread
- **CPU-side output** - the frame is composed directly in system memory, no GPU readback before OBS and preview
- **OpenCV SIMD** - GaussianBlur, resize, YuNet use AVX2/SSE
- **Frame pacing** - worker at camera FPS, UI at display refresh

---

## YuNet Face Detector

Model: `face_detection_yunet_2023mar.onnx` (included). Detects face box + 5 landmarks + confidence. Input: 320x240.

---

## License

MIT, see [LICENSE](LICENSE). Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
