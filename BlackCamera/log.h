#pragma once
// File logger. Release builds have no console: everything (including OpenCV's own
// warnings written to stdout/stderr) ends up in %LOCALAPPDATA%\BlackCamera\blackcamera.log.
// Debug builds additionally open a console window and mirror the log there.

#include <filesystem>

namespace logging {

// Opens the log file (previous run is kept as blackcamera.prev.log)
void init();
void shutdown();
std::filesystem::path logPath();

void write(char level, const char* fmt, ...);

} // namespace logging

#define LOG_INFO(...)  ::logging::write('I', __VA_ARGS__)
#define LOG_WARN(...)  ::logging::write('W', __VA_ARGS__)
#define LOG_ERROR(...) ::logging::write('E', __VA_ARGS__)
