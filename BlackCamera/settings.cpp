#include "settings.h"
#include "log.h"
#include "platform.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>

const CharsetPreset kCharsetPresets[] = {
    { "Sparse",          L"      .:-=+*#%@" },
    { "Standard",        L" .:-=+*#%@" },
    { "Classic Unicode", L" .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$" },
    { "Binary (0/1)",    L" 01" },
    { "Matrix Rain",     L" ｦｱｳｴｵｶｷｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝ1234567890:*+-<>|" },
    { "Custom...",       nullptr },
};
const int kCharsetPresetCount = (int)std::size(kCharsetPresets);

std::wstring Settings::charset() const {
    if (charsetPreset >= 0 && charsetPreset < kCharsetPresetCount && kCharsetPresets[charsetPreset].chars)
        return kCharsetPresets[charsetPreset].chars;
    std::wstring custom = platform::widen(customCharset);
    return custom.empty() ? L" .:+=#@" : custom;
}

void Settings::clamp() {
    brightness    = std::clamp(brightness, -100, 100);
    contrast      = std::clamp(contrast, 0.1f, 3.0f);
    blurSize      = std::clamp(blurSize, 1, 31) | 1; // odd
    minBrightness = std::clamp(minBrightness, 0, 255);
    colorMode     = std::clamp(colorMode, 0, 5);
    for (float& c : color) c = std::clamp(c, 0.0f, 1.0f);
    antialiasMode = std::clamp(antialiasMode, 0, 3);
    charsetPreset = std::clamp(charsetPreset, 0, kCharsetPresetCount - 1);
    if (customCharset.size() > 255) customCharset.resize(255);
    minFont       = std::clamp(minFont, 5.0f, 150.0f);
    maxFont       = std::clamp(maxFont, 30.0f, 150.0f);
    zoomSpeed     = std::clamp(zoomSpeed, 0.01f, 1.0f);
    hysteresis    = std::clamp(hysteresis, 0.0f, 0.3f);
    maxOccupancy  = std::clamp(maxOccupancy, 0.05f, 0.6f);
    minOccupancy  = std::clamp(minOccupancy, 0.005f, 0.2f);
}

std::filesystem::path settingsPath() {
    return platform::roamingDataDir() / L"settings.ini";
}

// ── Serialization ───────────────────────────────────────────────────────────
// One "key=value" per line. Values are taken verbatim after the first '='
// (the charset may start with spaces), numbers use the C locale.

namespace {

std::string fmt(float v) {
    char buf[32];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}

template <typename T>
void parse(const std::map<std::string, std::string>& kv, const char* key, T& out) {
    auto it = kv.find(key);
    if (it == kv.end()) return;
    T v{};
    const std::string& s = it->second;
    if (std::from_chars(s.data(), s.data() + s.size(), v).ec == std::errc()) out = v;
}

void parse(const std::map<std::string, std::string>& kv, const char* key, bool& out) {
    auto it = kv.find(key);
    if (it != kv.end()) out = (it->second == "1" || it->second == "true");
}

void parse(const std::map<std::string, std::string>& kv, const char* key, std::string& out) {
    auto it = kv.find(key);
    if (it != kv.end()) out = it->second;
}

} // namespace

Settings loadSettings(const std::filesystem::path& path) {
    Settings s;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LOG_INFO("No settings file yet, using defaults (%s)", platform::narrow(path.wstring()).c_str());
        return s;
    }

    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }

    parse(kv, "camera", s.cameraName);
    parse(kv, "brightness", s.brightness);
    parse(kv, "contrast", s.contrast);
    parse(kv, "blur", s.blurSize);
    parse(kv, "min_brightness", s.minBrightness);
    parse(kv, "color_mode", s.colorMode);
    parse(kv, "color_r", s.color[0]);
    parse(kv, "color_g", s.color[1]);
    parse(kv, "color_b", s.color[2]);
    parse(kv, "antialias", s.antialiasMode);
    parse(kv, "charset_preset", s.charsetPreset);
    parse(kv, "charset_custom", s.customCharset);
    parse(kv, "face_zoom", s.faceZoom);
    parse(kv, "min_font", s.minFont);
    parse(kv, "max_font", s.maxFont);
    parse(kv, "zoom_speed", s.zoomSpeed);
    parse(kv, "show_hud", s.showHud);
    parse(kv, "hysteresis", s.hysteresis);
    parse(kv, "max_occupancy", s.maxOccupancy);
    parse(kv, "min_occupancy", s.minOccupancy);
    s.clamp();

    LOG_INFO("Settings loaded from %s", platform::narrow(path.wstring()).c_str());
    return s;
}

bool saveSettings(const Settings& s, const std::filesystem::path& path) {
    std::ostringstream out;
    out << "# BlackCamera settings (UTF-8). Delete this file to restore defaults.\n"
        << "camera="         << s.cameraName << '\n'
        << "brightness="     << s.brightness << '\n'
        << "contrast="       << fmt(s.contrast) << '\n'
        << "blur="           << s.blurSize << '\n'
        << "min_brightness=" << s.minBrightness << '\n'
        << "color_mode="     << s.colorMode << '\n'
        << "color_r="        << fmt(s.color[0]) << '\n'
        << "color_g="        << fmt(s.color[1]) << '\n'
        << "color_b="        << fmt(s.color[2]) << '\n'
        << "antialias="      << s.antialiasMode << '\n'
        << "charset_preset=" << s.charsetPreset << '\n'
        << "charset_custom=" << s.customCharset << '\n'
        << "face_zoom="      << (s.faceZoom ? 1 : 0) << '\n'
        << "min_font="       << fmt(s.minFont) << '\n'
        << "max_font="       << fmt(s.maxFont) << '\n'
        << "zoom_speed="     << fmt(s.zoomSpeed) << '\n'
        << "show_hud="       << (s.showHud ? 1 : 0) << '\n'
        << "hysteresis="     << fmt(s.hysteresis) << '\n'
        << "max_occupancy="  << fmt(s.maxOccupancy) << '\n'
        << "min_occupancy="  << fmt(s.minOccupancy) << '\n';

    // Write a temp file and swap it in, so a crash never leaves a half-written file
    auto tmp = path;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { LOG_WARN("Cannot write settings file"); return false; }
        f << out.str();
        if (!f.flush()) { LOG_WARN("Cannot write settings file"); return false; }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOG_WARN("Cannot replace settings file (error %lu)", GetLastError());
        return false;
    }
    return true;
}
