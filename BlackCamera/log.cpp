#include "log.h"
#include "platform.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace logging {

static std::mutex s_mutex;
static FILE*      s_file = nullptr;
static bool       s_mirrorToConsole = false;
static std::filesystem::path s_path;

std::filesystem::path logPath() { return s_path; }

void init() {
    std::lock_guard lock(s_mutex);
    if (s_file) return;

    const auto dir = platform::localDataDir();
    s_path = dir / L"blackcamera.log";
    std::error_code ec;
    if (std::filesystem::exists(s_path, ec)) {
        std::filesystem::rename(s_path, dir / L"blackcamera.prev.log", ec);
    }
    s_file = _wfsopen(s_path.c_str(), L"a", _SH_DENYNO);

#ifdef _DEBUG
    // Debug: a console window for live output; OpenCV keeps printing there
    if (AllocConsole()) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        s_mirrorToConsole = true;
    }
#else
    // Release (no console): route stray stdout/stderr output, e.g. OpenCV warnings, into the log
    if (s_file) {
        FILE* f = nullptr;
        _wfreopen_s(&f, s_path.c_str(), L"a", stdout);
        _wfreopen_s(&f, s_path.c_str(), L"a", stderr);
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
#endif
}

void shutdown() {
    std::lock_guard lock(s_mutex);
    if (s_file) { fclose(s_file); s_file = nullptr; }
}

void write(char level, const char* fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1200];
    snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d [%c] %s\n",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level, msg);

    std::lock_guard lock(s_mutex);
    if (s_file) { fputs(line, s_file); fflush(s_file); }
    if (s_mirrorToConsole) { fputs(line, stderr); }
    OutputDebugStringA(line);
}

} // namespace logging
