#pragma once
// Small Windows helpers shared by all modules: string conversion and well-known folders.

#include <filesystem>
#include <string>

namespace platform {

std::wstring widen(const std::string& utf8);
std::string  narrow(const std::wstring& wide);

// Folder that contains BlackCamera.exe (bundled model lives there)
std::filesystem::path exeDir();

// %APPDATA%\BlackCamera — user settings (created on demand)
std::filesystem::path roamingDataDir();

// %LOCALAPPDATA%\BlackCamera — logs (created on demand)
std::filesystem::path localDataDir();

} // namespace platform
