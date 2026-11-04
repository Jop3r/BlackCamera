cmake_minimum_required(VERSION 3.25)

project(BlackCamera VERSION 1.2.0 DESCRIPTION "Real-time ASCII camera effect" LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "BlackCamera is Windows-only (Direct2D, Media Foundation, virtual cameras)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc videoio objdetect dnn)
find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/BlackCamera)
set(MODEL_FILE ${SRC_DIR}/face_detection_yunet_2023mar.onnx)

# Everything except the window/ImGui entry point, shared with the tests
add_library(blackcamera_core STATIC
    ${SRC_DIR}/platform.cpp
    ${SRC_DIR}/log.cpp
    ${SRC_DIR}/settings.cpp
    ${SRC_DIR}/camera.cpp
    ${SRC_DIR}/face_tracker.cpp
    ${SRC_DIR}/ascii_renderer.cpp
    ${SRC_DIR}/nv12.cpp
    ${SRC_DIR}/obs_vcam.cpp
    ${SRC_DIR}/mf_vcam.cpp
    ${SRC_DIR}/vcam_output.cpp
    ${SRC_DIR}/pipeline.cpp
)
target_include_directories(blackcamera_core PUBLIC ${SRC_DIR})
target_compile_definitions(blackcamera_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(blackcamera_core PUBLIC /utf-8 /W3 /permissive-)
target_link_libraries(blackcamera_core PUBLIC
    opencv_core opencv_imgproc opencv_videoio opencv_objdetect
    d2d1 dwrite windowscodecs mfplat mf mfuuid shell32 ole32 advapi32
)

add_executable(BlackCamera WIN32
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/gui.cpp
)
target_link_libraries(BlackCamera PRIVATE blackcamera_core glfw imgui::imgui OpenGL::GL)
# Keep the standard main() entry point in a GUI-subsystem (no console) executable
target_link_options(BlackCamera PRIVATE /ENTRY:mainCRTStartup)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT BlackCamera)

# The face model must sit next to the executable
add_custom_command(TARGET BlackCamera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MODEL_FILE} $<TARGET_FILE_DIR:BlackCamera>
)

# Media source of the built-in Windows 11 virtual camera. Loaded by the Camera
# Frame Server service (LOCAL SERVICE) and camera apps, registered with regsvr32.
# Static CRT and no vcpkg dependencies, so it loads in any of those processes.
set(VCAM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/BlackCameraVCam)
add_library(BlackCameraVCam SHARED
    ${VCAM_DIR}/dllmain.cpp
    ${VCAM_DIR}/activator.cpp
    ${VCAM_DIR}/media_source.cpp
    ${VCAM_DIR}/media_stream.cpp
    ${VCAM_DIR}/frame_reader.cpp
    ${VCAM_DIR}/BlackCameraVCam.def
)
target_include_directories(BlackCameraVCam PRIVATE ${VCAM_DIR} ${SRC_DIR}) # vcam_shared.h
target_compile_definitions(BlackCameraVCam PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(BlackCameraVCam PRIVATE /utf-8 /W3 /permissive-)
target_link_libraries(BlackCameraVCam PRIVATE mfplat mfuuid mfsensorgroup ole32 advapi32)
set_property(TARGET BlackCameraVCam PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# The app is useful without it (OBS output), but ship them together
add_dependencies(BlackCamera BlackCameraVCam)

include(CTest)
if(BUILD_TESTING)
    add_executable(settings_test tests/settings_test.cpp)
    target_link_libraries(settings_test PRIVATE blackcamera_core)
    add_test(NAME settings_test COMMAND settings_test)

    add_executable(vcam_protocol_test tests/vcam_protocol_test.cpp)
    target_link_libraries(vcam_protocol_test PRIVATE blackcamera_core)
    add_test(NAME vcam_protocol_test COMMAND vcam_protocol_test)

    # Reads BlackCameraVCam.dll in-process through IMFSourceReader (no registration needed)
    add_executable(vcam_source_test tests/vcam_source_test.cpp)
    target_include_directories(vcam_source_test PRIVATE ${SRC_DIR})
    target_compile_definitions(vcam_source_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(vcam_source_test PRIVATE /utf-8 /W3 /permissive-)
    target_link_libraries(vcam_source_test PRIVATE mfplat mfreadwrite mfuuid ole32)
    add_dependencies(vcam_source_test BlackCameraVCam)
    add_test(NAME vcam_source_test COMMAND vcam_source_test)
endif()

install(TARGETS BlackCamera RUNTIME DESTINATION .)
# Only the DLL itself, not its import library; installer.iss registers it
install(FILES $<TARGET_FILE:BlackCameraVCam> DESTINATION .)
# vcpkg's build step already copied exactly the DLLs the executable needs next to it
install(DIRECTORY $<TARGET_FILE_DIR:BlackCamera>/ DESTINATION . FILES_MATCHING PATTERN "*.dll")
install(FILES ${MODEL_FILE} LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
