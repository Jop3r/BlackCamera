/*
 * BlackCamera — Real-time ASCII camera effect
 * ── Glyph Atlas Renderer + Asynchronous Pipeline ─────────────────────────────
 *
 *   main.cpp           window, ImGui, preview texture
 *   gui.*              control panel, settings persistence trigger
 *   pipeline.*         worker thread: camera -> tracker -> renderer -> OBS
 *   camera.*           device enumeration by name, capture, offline screen
 *   face_tracker.*     YuNet detection, font zoom, HUD smoothing
 *   ascii_renderer.*   glyph atlas + CPU compositing, HUD drawing
 *   obs_vcam.*         OBS Virtual Camera shared-memory output
 *   settings.*         Settings struct, settings.ini load/save
 *   log.*, platform.*  file logging, Windows helpers
 */

#include "gui.h"
#include "log.h"
#include "pipeline.h"
#include "platform.h"
#include "settings.h"

#include <windows.h>
#include <objbase.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <GL/gl.h>

#include <algorithm>
#include <string>

// GL_CLAMP_TO_EDGE may not be defined in Windows' old GL/gl.h
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace {

constexpr int kOutputWidth = 1920;
constexpr int kOutputHeight = 1080;

void fatal(const char* message) {
    LOG_ERROR("%s", message);
    MessageBoxA(nullptr, message, "BlackCamera Error", MB_ICONERROR);
}

// Fullscreen, letterboxed preview behind the control panel
class PreviewTexture {
public:
    PreviewTexture() {
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    ~PreviewTexture() { glDeleteTextures(1, &tex_); }

    void upload(const cv::Mat& rgb) {
        glBindTexture(GL_TEXTURE_2D, tex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (rgb.cols != w_ || rgb.rows != h_) {
            w_ = rgb.cols;
            h_ = rgb.rows;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w_, h_, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.ptr());
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w_, h_, GL_RGB, GL_UNSIGNED_BYTE, rgb.ptr());
        }
    }

    void draw(int displayW, int displayH) const {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)displayW, (float)displayH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Preview", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs);
        if (w_ > 0 && h_ > 0) {
            float scale = std::min((float)displayW / w_, (float)displayH / h_);
            ImVec2 size(w_ * scale, h_ * scale);
            ImGui::SetCursorPos(ImVec2((displayW - size.x) * 0.5f, (displayH - size.y) * 0.5f));
            ImGui::Image((ImTextureID)(intptr_t)tex_, size);
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

private:
    GLuint tex_ = 0;
    int w_ = 0, h_ = 0;
};

} // namespace

int main() {
    logging::init();
    LOG_INFO("BlackCamera starting, log: %s", platform::narrow(logging::logPath().wstring()).c_str());
    const bool comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); // camera enumeration

    if (!glfwInit()) {
        fatal("Failed to initialize GLFW.");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1024, 768, "BlackCamera", nullptr, nullptr);
    if (!window) {
        fatal("Failed to create the OpenGL window.");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Window layout next to the settings, not in the (possibly read-only) install folder
    static const std::string imguiIni = platform::narrow((platform::roamingDataDir() / L"imgui.ini").wstring());
    ImGui::GetIO().IniFilename = imguiIni.c_str();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    SettingsStore settings(loadSettings());
    Pipeline pipeline(settings, kOutputWidth, kOutputHeight);
    pipeline.start();

    {
        PreviewTexture preview;
        ControlPanel panel(settings, pipeline);
        cv::Mat frame;

        while (!glfwWindowShouldClose(window) && pipeline.alive()) {
            glfwPollEvents();
            if (pipeline.takePreview(frame)) preview.upload(frame);

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            int displayW, displayH;
            glfwGetFramebufferSize(window, &displayW, &displayH);
            glViewport(0, 0, displayW, displayH);

            preview.draw(displayW, displayH);
            panel.draw();

            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }

        if (!pipeline.alive()) fatal("The render pipeline stopped. See the log for details.");
        panel.flush();
        pipeline.stop();
    } // GL texture is released while the context is still alive

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    if (comInitialized) CoUninitialize();

    LOG_INFO("BlackCamera exited");
    logging::shutdown();
    return 0;
}
