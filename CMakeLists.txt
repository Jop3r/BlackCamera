cmake_minimum_required(VERSION 3.25)

project(BlackCamera VERSION 1.1.0 DESCRIPTION "Real-time ASCII camera effect" LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "BlackCamera is Windows-only (Direct2D, Media Foundation, OBS Virtual Camera)")
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
    ${SRC_DIR}/obs_vcam.cpp
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

include(CTest)
if(BUILD_TESTING)
    add_executable(settings_test tests/settings_test.cpp)
    target_link_libraries(settings_test PRIVATE blackcamera_core)
    add_test(NAME settings_test COMMAND settings_test)
endif()

install(TARGETS BlackCamera RUNTIME DESTINATION .)
# vcpkg's build step already copied exactly the DLLs the executable needs next to it
install(DIRECTORY $<TARGET_FILE_DIR:BlackCamera>/ DESTINATION . FILES_MATCHING PATTERN "*.dll")
install(FILES ${MODEL_FILE} LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
