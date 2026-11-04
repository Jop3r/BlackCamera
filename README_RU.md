# BlackCamera — C++ версия

Эффект ASCII-камеры в реальном времени с рендером через атлас глифов (DirectWrite + многопоточная сборка кадра на CPU). C++ порт test.py с существенным приростом производительности.

[🇺🇸 English version](README.md)

[![build](https://github.com/Jop3r/BlackCamera/actions/workflows/build.yml/badge.svg)](https://github.com/Jop3r/BlackCamera/actions/workflows/build.yml)

---

## Сборка

Нужно: Windows 10/11 x64 (встроенной виртуальной камере нужна Windows 11), Visual Studio 2022 или новее с нагрузкой **Разработка классических приложений на C++**, [vcpkg](https://github.com/microsoft/vcpkg). Зависимости (OpenCV, GLFW, Dear ImGui) описаны в `vcpkg.json` и ставятся автоматически.

### CMake (рекомендуется, так же собирает CI)

```bat
set VCPKG_ROOT=%USERPROFILE%\vcpkg
cmake --workflow --preset release
```

Команда конфигурирует проект, собирает его и запускает тесты. Exe и `BlackCameraVCam.dll` появятся в `build\Release\`. Первая сборка компилирует OpenCV через vcpkg и может занять время; дальше используется бинарный кэш vcpkg.

Visual Studio может открыть папку напрямую (**Файл → Открыть → Папка**) и использовать тот же пресет.

### Решение Visual Studio

`BlackCamera.slnx` использует классическую раскладку vcpkg (`%USERPROFILE%\vcpkg\installed\x64-windows`), поэтому пакеты нужно один раз поставить:

```bat
%USERPROFILE%\vcpkg\vcpkg.exe install opencv4 glfw3 imgui[glfw-binding,opengl3-binding] --triplet x64-windows
```

Затем открыть `BlackCamera.slnx`, выбрать **Release | x64** и собрать. Post-build шаг копирует DLL и модель лица рядом с .exe.

### Установщик

```bat
powershell -ExecutionPolicy Bypass -File tools\make-installer.ps1
```

Собирает Release, раскладывает его в `dist\BlackCamera` и компилирует `dist\BlackCamera_Setup_v<версия>.exe` через [Inno Setup 6](https://jrsoftware.org/isdl.php). Установщик включает Microsoft Visual C++ Redistributable и на Windows 11 регистрирует `BlackCameraVCam.dll` (при удалении регистрация снимается).

---

## Запуск

Запустить `BlackCamera.exe`. Откроется панель управления поверх полноэкранного превью; для выхода закрыть окно. Консольного окна нет — см. [Логи](#логи).

### Камера

Вебкамера выбирается по имени в списке **Camera**. Выбор запоминается по имени, поэтому переживает переподключение и смену порядка устройств. Если выбранной камеры нет, BlackCamera берёт первую доступную и возвращается к выбранной, как только она снова появится. **OBS Virtual Camera** и **BlackCamera** никогда не используются как вход: это собственные выходы BlackCamera.

### Виртуальная камера

Картинка BlackCamera попадает в другие программы через виртуальную камеру. Выход выбирается в списке **Output** (сохраняется в `settings.ini`):

| Выход | Что нужно | Имя камеры в программах |
|-------|-----------|-------------------------|
| **Auto** (по умолчанию) | — | Windows 11: `BlackCamera`, иначе `OBS Virtual Camera` |
| **Windows 11 built-in camera** | Windows 11 (сборка 22000+), BlackCamera установлен установщиком | `BlackCamera` |
| **OBS Virtual Camera** | установленный OBS Studio | `OBS Virtual Camera` |

**Auto** использует встроенную камеру, а если она недоступна (Windows 10, источник не зарегистрирован, ошибка регистрации), переключается на OBS. Причина пишется в [лог](#логи).

#### Встроенная камера Windows 11 (без OBS)

1. Установить BlackCamera установщиком. Он регистрирует `BlackCameraVCam.dll` — медиаисточник камеры (нужны права администратора, один раз).
2. Запустить `BlackCamera.exe`. В панели появится `Windows Camera "BlackCamera": READY`.
3. Выбрать **BlackCamera** в любом видеоприложении (Камера Windows, Teams, Zoom, Discord, Chrome/Edge). Пока программа читает камеру, статус — `STREAMING`.

Камера существует, только пока запущен BlackCamera: она создаётся через `MFCreateVirtualCamera` для текущего пользователя и удаляется при выходе. Камеру могут одновременно использовать несколько программ. Если открыть её, когда BlackCamera не отдаёт кадры, программа покажет заставку «BLACKCAMERA — APP IS NOT RUNNING». Форматы: NV12 и YUY2, 1920x1080, 30 FPS. Драйвер ядра не нужен: DLL медиаисточника загружает служба Windows «Сервер кадров камеры» (Frame Server), а приложение передаёт ей кадры через общую память.

Статус в панели управления:

- `NOT REGISTERED` — `BlackCameraVCam.dll` не зарегистрирована. Переустановите BlackCamera; для сборки из исходников выполните `regsvr32 BlackCameraVCam.dll` от администратора из папки, которую может читать учётная запись `LOCAL SERVICE` (не внутри `C:\Users\<имя>`).
- `NEEDS WINDOWS 11` — на Windows 10 используйте выход OBS.
- `REGISTRATION FAILED` — обычно доступ к камере выключен в **Параметры → Конфиденциальность и защита → Камера**; BlackCamera повторяет попытку каждые 5 с.

#### OBS Virtual Camera

1. Установить [OBS Studio](https://obsproject.com/). Запускать сам OBS **не нужно**.
2. Убедиться, что виртуальная камера в OBS **не** запущена (**Start Virtual Camera** выключено) — эту роль берёт на себя BlackCamera.
3. Запустить `BlackCamera.exe`. В панели появится `OBS Virtual Camera: STREAMING`.
4. Выбрать **OBS Virtual Camera** в любом видеоприложении (Zoom, Discord, Telegram, браузер).

Статус в панели управления:

- `BUSY` — виртуальная камера уже запущена в OBS. Остановите её там; BlackCamera повторяет попытку каждые 2 с.
- `NOT INSTALLED` — OBS Studio не установлен. BlackCamera всё равно работает с окном превью.

Виртуальная камера у OBS одна, поэтому захватить картинку BlackCamera в OBS и вывести её обратно через ту же камеру нельзя.

---

## Настройки

Все контролы панели сохраняются автоматически (примерно через секунду после последнего изменения) в `%APPDATA%\BlackCamera\settings.ini`. **Restore Default Settings** сбрасывает всё, кроме выбранной камеры и выхода; удаление файла делает то же при следующем запуске. Раскладка окон панели хранится рядом в `imgui.ini`.

- **Camera** — устройство по имени или Auto
- **Output** — Auto / Windows 11 built-in camera / OBS Virtual Camera
- **Brightness** (-100..100) — аддитивное смещение
- **Contrast** (0.1..3.0) — мультипликативный коэффициент
- **Antialiasing** — Default / ClearType / Grayscale / Aliased (retro sharp)
- **Blur Kernel Size** (1..31, нечётное)
- **Color Mode** — Custom / Camera / Matrix Green / Amber CRT / Cyberpunk / Rainbow
- **Character Preset** — Sparse / Standard / Classic Unicode / Binary / Matrix Rain / Custom
- **Min Brightness Threshold** (0..255)
- **Face Tracking Font Zoom** — вкл/выкл + Min/Max шрифт, скорость зума
- **Cyber HUD** — рамка лица, точки лица, статус-оверлей
- **Advanced Stabilization** — Jitter Deadzone, Max Sensitivity, Min Threshold

Разрешение выхода фиксированное: 1920x1080, 30 FPS.

## Логи

`%LOCALAPPDATA%\BlackCamera\blackcamera.log` (предыдущий запуск сохраняется как `blackcamera.prev.log`). В логе: найденные камеры, используемая камера, статус виртуальной камеры (в том числе почему Auto переключился на OBS) и предупреждения OpenCV. Debug-сборка дополнительно открывает консоль с тем же выводом.

---

## Структура исходников

| Файл | Отвечает за |
|------|-------------|
| `main.cpp` | Окно, ImGui, текстура превью |
| `gui.*` | Панель управления, автосохранение настроек |
| `pipeline.*` | Рабочий поток: камера → трекер лица → рендер → виртуальная камера |
| `camera.*` | Список устройств по имени (Media Foundation), захват, экран «нет сигнала» |
| `face_tracker.*` | Детекция YuNet, зум шрифта, сглаживание HUD |
| `ascii_renderer.*` | Атлас глифов, сборка кадра, отрисовка HUD |
| `vcam_output.*` | Выбор выхода (Auto / Windows / OBS) и переключение на запасной |
| `mf_vcam.*` | Виртуальная камера Windows 11: `MFCreateVirtualCamera`, кадры в общую память |
| `vcam_shared.h` | Протокол общей памяти между приложением и `BlackCameraVCam.dll` |
| `obs_vcam.*` | Выход в OBS Virtual Camera через shared memory |
| `nv12.*` | Преобразование BGR → NV12 для обоих выходов |
| `settings.*` | Структура настроек, загрузка/сохранение `settings.ini` |
| `log.*`, `platform.*` | Логирование в файл, утилиты Windows |

`BlackCameraVCam/` — DLL медиаисточника виртуальной камеры (внутрипроцессный COM-сервер: `IMFActivate`, `IMFMediaSourceEx`, `IMFMediaStream2`, `IKsControl`), её загружает служба Frame Server. Сторонних зависимостей у неё нет, собирается она только через CMake; решение Visual Studio собирает одно приложение.

---

## Почему быстрее Python?

- **Нет GIL** — настоящий параллелизм
- **Атлас глифов** — каждый символ растеризуется DirectWrite один раз, кадр собирается из готовых масок (без раскладки текста на каждом кадре)
- **Async pipeline** — захват/рендер в рабочем потоке, UI в главном
- **Кадр сразу в памяти** — нет чтения с GPU перед выводом в OBS и превью
- **OpenCV SIMD** — GaussianBlur, resize, YuNet используют AVX2/SSE
- **Frame pacing** — воркер на FPS камеры, UI на частоте обновления экрана

---

## YuNet Face Detector

Модель: `face_detection_yunet_2023mar.onnx` (включена). Детектирует бокс лица + 5 лендмарков + confidence. Вход: 320x240.

---

## Лицензия

MIT, см. [LICENSE](LICENSE). Сторонние компоненты: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
