#include "gui.h"
#include "log.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr auto kSaveDelay = std::chrono::seconds(1);

const ImVec4 kGreen(0.0f, 1.0f, 0.0f, 1.0f);
const ImVec4 kOrange(1.0f, 0.7f, 0.2f, 1.0f);
const ImVec4 kRed(1.0f, 0.3f, 0.3f, 1.0f);
const ImVec4 kGray(0.7f, 0.7f, 0.7f, 1.0f);

void copyToBuffer(char (&dst)[256], const std::string& src) {
    strncpy_s(dst, src.c_str(), _TRUNCATE);
}

} // namespace

ControlPanel::ControlPanel(SettingsStore& store, Pipeline& pipeline)
    : store_(store), pipeline_(pipeline), ui_(store.get()), pushed_(ui_) {
    copyToBuffer(customCharset_, ui_.customCharset);
    refreshCameras();
}

void ControlPanel::refreshCameras() {
    const bool first = cameras_.empty();
    cameras_ = enumerateCameras();
    if (first) {
        for (const auto& cam : cameras_)
            LOG_INFO("Camera device %d: \"%s\"%s", cam.index, cam.name.c_str(), cam.isOwnOutput ? " (own output, ignored)" : "");
    }
}

void ControlPanel::drawCameraSection() {
    ImGui::Text("Webcam Device Input");

    const char* preview = ui_.cameraName.empty() ? "Auto (first available)" : ui_.cameraName.c_str();
    if (ImGui::BeginCombo("Camera", preview)) {
        if (ImGui::IsWindowAppearing()) refreshCameras(); // fresh list every time the combo opens

        if (ImGui::Selectable("Auto (first available)", ui_.cameraName.empty())) ui_.cameraName.clear();
        bool selectedListed = ui_.cameraName.empty();
        for (const auto& cam : cameras_) {
            ImGui::PushID(cam.index);
            if (cam.isOwnOutput) {
                // Our own output: selecting it would feed the effect back into itself
                ImGui::BeginDisabled();
                ImGui::Selectable((cam.name + "  (BlackCamera output)").c_str(), false);
                ImGui::EndDisabled();
            } else if (ImGui::Selectable(cam.name.c_str(), cam.name == ui_.cameraName)) {
                ui_.cameraName = cam.name;
            }
            selectedListed |= (cam.name == ui_.cameraName);
            ImGui::PopID();
        }
        if (!selectedListed) {
            ImGui::BeginDisabled();
            ImGui::Selectable((ui_.cameraName + "  (not connected)").c_str(), true);
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }

    const std::string active = pipeline_.activeCamera();
    if (active.empty()) {
        ImGui::TextColored(kRed, "Webcam Status: OFFLINE / WAITING FOR DEVICE...");
    } else if (!ui_.cameraName.empty() && active != ui_.cameraName) {
        ImGui::TextColored(kOrange, "Selected camera not found, using: %s", active.c_str());
    } else {
        ImGui::TextColored(kGreen, "Webcam Status: ACTIVE (%s)", active.c_str());
    }
}

void ControlPanel::draw() {
    ImGui::Begin("BlackCamera Control Panel", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    if (ImGui::Button("Restore Default Settings")) {
        const std::string camera = ui_.cameraName; // the device choice is not a "look" setting
        ui_ = Settings{};
        ui_.cameraName = camera;
        copyToBuffer(customCharset_, ui_.customCharset);
    }
    ImGui::Separator();

    ImGui::Text("Glyph Atlas Renderer (DirectWrite + CPU)");
    ImGui::Separator();

    drawCameraSection();
    ImGui::Separator();

    ImGui::SliderInt("Brightness", &ui_.brightness, -100, 100);
    ImGui::SliderFloat("Contrast", &ui_.contrast, 0.1f, 3.0f);

    ImGui::Separator();
    ImGui::Text("Text Rendering Options");
    const char* aaModes[] = { "Default", "ClearType (Smooth)", "Grayscale (Anti-aliased)", "Aliased (Retro Sharp)" };
    ImGui::Combo("Antialiasing", &ui_.antialiasMode, aaModes, IM_ARRAYSIZE(aaModes));

    ImGui::Separator();
    ImGui::Text("Blur / Preprocessing");
    if (ImGui::SliderInt("Blur Kernel Size", &ui_.blurSize, 1, 31)) ui_.blurSize |= 1; // must be odd

    ImGui::Separator();
    ImGui::Text("Color Options & Themes");
    const char* colorModes[] = { "Custom Monochrome", "Camera Color", "Matrix Green", "Amber CRT (Retro)", "Cyberpunk Gradient", "Rainbow Wave" };
    ImGui::Combo("Color Mode", &ui_.colorMode, colorModes, IM_ARRAYSIZE(colorModes));
    if (ui_.colorMode == 0) ImGui::ColorEdit3("ASCII Color", ui_.color.data());

    ImGui::Separator();
    ImGui::Text("ASCII Presets & Density");
    const char* presetNames[16];
    for (int i = 0; i < kCharsetPresetCount; ++i) presetNames[i] = kCharsetPresets[i].name;
    ImGui::Combo("Character Preset", &ui_.charsetPreset, presetNames, kCharsetPresetCount);
    if (kCharsetPresets[ui_.charsetPreset].chars == nullptr) {
        if (ImGui::InputText("Custom Palette", customCharset_, IM_ARRAYSIZE(customCharset_)))
            ui_.customCharset = customCharset_;
        ImGui::TextColored(kGray, "Tip: Order symbols from darkest to brightest.");
    }
    ImGui::SliderInt("Min Brightness Threshold", &ui_.minBrightness, 0, 255);

    ImGui::Separator();
    ImGui::Text("Face Tracking & Font Zoom");
    ImGui::Checkbox("Enable Face Tracking Font Zoom", &ui_.faceZoom);
    if (ui_.faceZoom) {
        ImGui::SliderFloat("Min Font Size", &ui_.minFont, 5.0f, 30.0f);
        ImGui::SliderFloat("Max Font Size", &ui_.maxFont, 30.0f, 150.0f);
        ImGui::SliderFloat("Zoom Speed", &ui_.zoomSpeed, 0.01f, 1.0f);

        ImGui::Separator();
        ImGui::Text("HUD & Advanced Stabilization Settings");
        ImGui::Checkbox("Show Cyber HUD Overlay", &ui_.showHud);

        float hyst = ui_.hysteresis * 100.0f;
        if (ImGui::SliderFloat("Zoom Jitter Deadzone (%)", &hyst, 0.0f, 30.0f, "%.1f%%")) ui_.hysteresis = hyst / 100.0f;
        float maxOcc = ui_.maxOccupancy * 100.0f;
        if (ImGui::SliderFloat("Max Distance Sensitivity (%)", &maxOcc, 5.0f, 60.0f, "%.1f%%")) ui_.maxOccupancy = maxOcc / 100.0f;
        float minOcc = ui_.minOccupancy * 100.0f;
        if (ImGui::SliderFloat("Min Zoom Distance Threshold (%)", &minOcc, 0.5f, 20.0f, "%.1f%%")) ui_.minOccupancy = minOcc / 100.0f;
    } else {
        ImGui::SliderFloat("Font Size", &ui_.minFont, 5.0f, 150.0f);
    }

    ImGui::Separator();
    switch (pipeline_.vcamStatus()) {
        case VcamStatus::Streaming:    ImGui::TextColored(kGreen, "OBS Virtual Camera: STREAMING"); break;
        case VcamStatus::Busy:         ImGui::TextColored(kOrange, "OBS Virtual Camera: BUSY (stop it in OBS)"); break;
        case VcamStatus::NotInstalled: ImGui::TextColored(kRed, "OBS Virtual Camera: NOT INSTALLED"); break;
        case VcamStatus::Error:        ImGui::TextColored(kRed, "OBS Virtual Camera: ERROR (retrying)"); break;
        default:                       ImGui::TextColored(kGray, "OBS Virtual Camera: OFF"); break;
    }
    ImGui::Text("Worker Engine FPS: %.1f", pipeline_.workerFps());
    ImGui::Text("GUI Render FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::End();

    // Hand changes to the worker immediately, persist them once editing pauses
    const auto now = std::chrono::steady_clock::now();
    if (!(ui_ == pushed_)) {
        ui_.clamp();
        store_.set(ui_);
        pushed_ = ui_;
        dirty_ = true;
        lastEdit_ = now;
    }
    if (dirty_ && now - lastEdit_ > kSaveDelay) flush();
}

void ControlPanel::flush() {
    if (!dirty_) return;
    if (saveSettings(pushed_)) dirty_ = false;
    else lastEdit_ = std::chrono::steady_clock::now(); // retry after the next delay, not every frame
}
