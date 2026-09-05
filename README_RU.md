# BlackCamera — C++ версия

Эффект ASCII-камеры в реальном времени с аппаратным ускорением Direct2D. C++ порт test.py с существенным приростом производительности.

[🇺🇸 English version](README.md)

---

## Зависимости

| Библиотека | Установка |
|------------|-----------|
| OpenCV 4.x | `%USERPROFILE%\vcpkg\vcpkg.exe` install opencv4:x64-windows |
| GLFW3 | `%USERPROFILE%\vcpkg\vcpkg.exe` install glfw3:x64-windows |
| Dear ImGui | `%USERPROFILE%\vcpkg\vcpkg.exe` install imgui[glfw-binding,opengl3-binding]:x64-windows |
| vcpkg | Уже установлен в `%USERPROFILE%\vcpkg` |

---

## Сборка

1. Открыть `BlackCamera.slnx` в Visual Studio 2022.
2. Выбрать **Release | x64** (или Debug | x64).
3. **Build → Build Solution** (Ctrl+Shift+B).

Post-build шаг копирует только нужные OpenCV + GLFW DLL рядом с .exe.

---

## Запуск

```
BlackCamera.exe
```

Откроется панель управления ImGui + полноэкранное превью. Закрыть окно для выхода.

### Виртуальная камера (OBS)

1. Запустить OBS Studio.
2. **Tools → Start Virtual Camera**.
3. Запустить `BlackCamera.exe` — он автоматически обнаружит shared memory OBS и будет стримить туда.
4. Выбрать **OBS Virtual Camera** в любом видеоприложении.

Если OBS не запущен, BlackCamera работает автономно с окном превью.

---

## Настройки (в начале main.cpp)

| Константа | По умолчанию | Описание |
|-----------|-------------|----------|
| CAM_INDEX | 0 | Индекс веб-камеры |
| OUT_W / OUT_H | 1920 / 1080 | Разрешение выхода |
| MIN_FONT_SZ | 10.0f | Мин. размер шрифта |
| MAX_FONT_SZ | 45.0f | Макс. размер шрифта |
| GUI_MaxOccupancy | 0.15f | Доля лица для макс. зума |
| GUI_MinOccupancy | 0.015f | Доля лица для мин. зума |
| TARGET_FPS | 60 | Целевой FPS захвата |
| GUI_BlurSize | 3 | Ядро Гаусса (нечётное) |
| GUI_FaceHysteresis | 0.06f | Dead-zone зума (6%) |
| GUI_FaceZoomSpeed | 0.2f | Скорость интерполяции шрифта |
| GUI_Color | {0,1,0.2} | Кастомный монохромный цвет (RGB 0-1) |

### Runtime-контролы в ImGui

- **Brightness** (-100..100) — аддитивное смещение
- **Contrast** (0.1..3.0) — мультипликативный коэффициент
- **Color Mode** — Custom / Camera / Matrix Green / Amber CRT / Cyberpunk / Rainbow
- **Antialiasing** — Default / ClearType / Grayscale / Aliased (retro sharp)
- **Character Preset** — Sparse / Standard / Classic Unicode / Binary / Matrix Rain / Custom
- **Min Brightness Threshold** (0..255)
- **Face Tracking Font Zoom** — вкл/выкл + Min/Max шрифт, Скорость зума
- **Cyber HUD** — переключение бокса лица, лендмарков, статус-оверлея
- **Advanced Stabilization** — Jitter Deadzone, Max Sensitivity, Min Threshold

---

## Почему быстрее Python?

- **Нет GIL** — настоящий параллелизм
- **Direct2D GPU rendering** — аппаратный рендеринг текста
- **Async pipeline** — захват/рендер в рабочем потоке, UI в главном
- **Persistent resources** — D2D bitmap, кисти, text layouts переиспользуются
- **OpenCV SIMD** — GaussianBlur, resize, YuNet используют AVX2/SSE
- **Frame pacing** — воркер на FPS камеры, UI на частоте обновления экрана

---

## YuNet Face Detector

Модель: `face_detection_yunet_2023mar.onnx` (включена). Детектирует бокс лица + 5 лендмарков + confidence. Вход: 320x240.

---

## Лицензия

MIT