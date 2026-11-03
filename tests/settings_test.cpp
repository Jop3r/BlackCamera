// Round-trip and validation tests for settings.ini (run by CTest / CI).
#include "settings.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

static int g_failures = 0;
#define CHECK(cond) \
    do { if (!(cond)) { std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "blackcamera_settings_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "settings.ini";

    // Missing file -> defaults
    CHECK(loadSettings(file) == Settings{});

    // Round trip of every field, including tricky strings
    Settings s;
    s.cameraName    = "Galaxy S21 FE 5G (Виртуальная камера Windows)";
    s.brightness    = -42;
    s.contrast      = 1.75f;
    s.blurSize      = 7;
    s.minBrightness = 12;
    s.colorMode     = 5;
    s.color         = { 0.25f, 0.5f, 0.75f };
    s.antialiasMode = 1;
    s.charsetPreset = kCharsetPresetCount - 1;
    s.customCharset = "  .=#ж@";       // leading spaces, '=' and UTF-8 must survive
    s.faceZoom      = false;
    s.minFont       = 12.5f;
    s.maxFont       = 90.0f;
    s.zoomSpeed     = 0.33f;
    s.showHud       = false;
    s.hysteresis    = 0.1f;
    s.maxOccupancy  = 0.3f;
    s.minOccupancy  = 0.02f;
    CHECK(saveSettings(s, file));
    CHECK(loadSettings(file) == s);
    CHECK(loadSettings(file).charset() == L"  .=#ж@");

    // Out-of-range and garbage values are clamped or ignored
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << "contrast=99\nblur=4\ncolor_mode=17\nmin_font=abc\nunknown_key=1\nno equals sign\n";
    }
    const Settings bad = loadSettings(file);
    CHECK(bad.contrast == 3.0f);
    CHECK(bad.blurSize == 5);
    CHECK(bad.colorMode == 5);
    CHECK(bad.minFont == Settings{}.minFont);

    // Empty custom charset falls back to a usable palette
    Settings empty;
    empty.charsetPreset = kCharsetPresetCount - 1;
    empty.customCharset.clear();
    CHECK(!empty.charset().empty());

    fs::remove_all(dir);
    if (g_failures == 0) std::puts("settings_test: all checks passed");
    return g_failures == 0 ? 0 : 1;
}
