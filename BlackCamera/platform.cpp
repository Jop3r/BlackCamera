#include "platform.h"

#include <windows.h>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib") // SHGetKnownFolderPath
#pragma comment(lib, "ole32.lib")   // CoTaskMemFree

namespace platform {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::filesystem::path exeDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (len < buf.size()) { buf.resize(len); break; }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path();
}

static std::filesystem::path knownFolderSubdir(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) {
        dir = std::filesystem::path(raw) / L"BlackCamera";
    } else {
        dir = exeDir(); // portable fallback
    }
    CoTaskMemFree(raw);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path roamingDataDir() { return knownFolderSubdir(FOLDERID_RoamingAppData); }
std::filesystem::path localDataDir()   { return knownFolderSubdir(FOLDERID_LocalAppData); }

} // namespace platform
