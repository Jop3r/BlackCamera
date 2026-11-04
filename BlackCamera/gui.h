#pragma once
// ImGui control panel. Holds the editable copy of the settings, pushes every
// change to the SettingsStore and writes settings.ini shortly after edits stop.

#include "camera.h"
#include "pipeline.h"
#include "settings.h"

#include <chrono>
#include <vector>

class ControlPanel {
public:
    ControlPanel(SettingsStore& store, Pipeline& pipeline);

    void draw();
    // Writes pending changes to disk (call on exit)
    void flush();

private:
    void drawCameraSection();
    void drawOutputSection();
    void refreshCameras();

    SettingsStore& store_;
    Pipeline& pipeline_;

    Settings ui_;       // what the widgets edit
    Settings pushed_;   // last value handed to the store
    bool dirty_ = false;
    std::chrono::steady_clock::time_point lastEdit_;

    char customCharset_[256] = {};
    std::vector<CameraDevice> cameras_;
};
