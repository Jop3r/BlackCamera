#pragma once
// User settings: plain value struct, persisted as UTF-8 key=value text in
// %APPDATA%\BlackCamera\settings.ini. The GUI owns the editable copy and pushes
// changes into SettingsStore; the worker thread takes a snapshot every frame.

#include <array>
#include <filesystem>
#include <mutex>
#include <string>

struct Settings {
    // Camera
    std::string cameraName;              // friendly device name; empty = first available camera

    // Output
    int   vcamBackend   = 0;             // VcamBackend: 0 Auto (Windows 11 camera, else OBS), 1 Windows, 2 OBS

    // Image preprocessing
    int   brightness    = 0;             // additive offset [-100, 100]
    float contrast      = 1.0f;          // multiplier [0.1, 3.0]
    int   blurSize      = 3;             // Gaussian kernel, odd [1, 31]
    int   minBrightness = 30;            // cells darker than this stay empty [0, 255]

    // Text rendering
    int   colorMode     = 2;             // 0 Custom, 1 Camera, 2 Matrix Green, 3 Amber CRT, 4 Cyberpunk, 5 Rainbow
    std::array<float, 3> color { 0.0f, 1.0f, 0.2f }; // custom monochrome color, RGB 0..1
    int   antialiasMode = 3;             // 0 Default, 1 ClearType, 2 Grayscale, 3 Aliased
    int   charsetPreset = 1;             // index into kCharsetPresets
    std::string customCharset = " .:-=+*#%@"; // UTF-8, used by the "Custom..." preset

    // Face tracking font zoom
    bool  faceZoom      = true;
    float minFont       = 10.0f;         // also the fixed font size when face zoom is off
    float maxFont       = 45.0f;
    float zoomSpeed     = 0.2f;
    bool  showHud       = true;
    float hysteresis    = 0.06f;         // zoom dead zone, fraction of current face size
    float maxOccupancy  = 0.15f;         // face area fraction that gives the max font
    float minOccupancy  = 0.015f;        // face area fraction below which zoom stays at min

    bool operator==(const Settings&) const = default;

    // Characters of the active preset, darkest -> brightest
    std::wstring charset() const;
    // Forces every field into its valid range
    void clamp();
};

struct CharsetPreset {
    const char*    name;
    const wchar_t* chars; // nullptr = custom
};
extern const CharsetPreset kCharsetPresets[];
extern const int kCharsetPresetCount;

std::filesystem::path settingsPath();   // %APPDATA%\BlackCamera\settings.ini
// Defaults when the file is missing or broken
Settings loadSettings(const std::filesystem::path& path = settingsPath());
// Atomic replace (temp file + rename)
bool saveSettings(const Settings& s, const std::filesystem::path& path = settingsPath());

class SettingsStore {
public:
    explicit SettingsStore(Settings initial) : settings_(std::move(initial)) {}
    Settings get() const { std::lock_guard lock(mutex_); return settings_; }
    void set(const Settings& s) { std::lock_guard lock(mutex_); settings_ = s; }

private:
    mutable std::mutex mutex_;
    Settings settings_;
};
