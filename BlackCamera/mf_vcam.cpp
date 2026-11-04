#include "mf_vcam.h"
#include "log.h"
#include "vcam_shared.h"

#include <windows.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <future>
#include <string>

#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint64_t kMappingRetryMs = 500;

// mfsensorgroup.dll exists on Windows 10 too, but without the virtual camera
// exports, so they are resolved at run time instead of being imported.
using CreateVirtualCameraFn = HRESULT(WINAPI*)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess,
                                               LPCWSTR, LPCWSTR, const GUID*, ULONG, IMFVirtualCamera**);
using IsTypeSupportedFn = HRESULT(WINAPI*)(MFVirtualCameraType, BOOL*);

HMODULE sensorGroupDll() {
    static HMODULE dll = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return dll;
}

template <typename Fn>
Fn sensorGroupProc(const char* name) {
    HMODULE dll = sensorGroupDll();
    return dll ? reinterpret_cast<Fn>(GetProcAddress(dll, name)) : nullptr;
}

std::string hrText(HRESULT hr) {
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)hr, 0, buf, sizeof(buf), nullptr);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

} // namespace

bool MfVirtualCamOutput::isSupported() {
    auto isTypeSupported = sensorGroupProc<IsTypeSupportedFn>("MFIsVirtualCameraTypeSupported");
    if (!isTypeSupported || !sensorGroupProc<CreateVirtualCameraFn>("MFCreateVirtualCamera")) return false;
    BOOL supported = FALSE;
    return SUCCEEDED(isTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &supported)) && supported;
}

bool MfVirtualCamOutput::isSourceRegistered() {
    const std::wstring path = std::wstring(L"Software\\Classes\\CLSID\\") + vcam::kSourceClsidString + L"\\InprocServer32";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return false;
    wchar_t dll[MAX_PATH] = {};
    DWORD size = sizeof(dll) - sizeof(wchar_t);
    const bool ok = RegQueryValueExW(key, nullptr, nullptr, nullptr, reinterpret_cast<BYTE*>(dll), &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok && GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES; // a stale entry from a removed install does not count
}

VcamStatus MfVirtualCamOutput::start() {
    stop();
    if (!isSupported()) return VcamStatus::Unsupported;
    if (!isSourceRegistered()) return VcamStatus::NotInstalled;

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return VcamStatus::Error;

    // The camera object lives on its own MTA thread: the worker thread is an
    // STA without a message loop, and the object must outlive single calls.
    std::promise<HRESULT> started;
    std::future<HRESULT> result = started.get_future();
    HANDLE stopEvent = stopEvent_;
    worker_ = std::thread([stopEvent, promise = std::move(started)]() mutable {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        const bool mf = SUCCEEDED(MFStartup(MF_VERSION));
        HRESULT hr = E_FAIL;
        ComPtr<IMFVirtualCamera> camera;
        if (auto create = sensorGroupProc<CreateVirtualCameraFn>("MFCreateVirtualCamera")) {
            hr = create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                        MFVirtualCameraAccess_CurrentUser, vcam::kFriendlyName, vcam::kSourceClsidString,
                        nullptr, 0, &camera);
            if (SUCCEEDED(hr)) hr = camera->Start(nullptr);
        }
        promise.set_value(hr);

        if (SUCCEEDED(hr)) {
            WaitForSingleObject(stopEvent, INFINITE);
            const HRESULT removed = camera->Remove();
            if (FAILED(removed)) LOG_WARN("Windows virtual camera: Remove failed (0x%08lX)", (unsigned long)removed);
        }
        camera.Reset(); // session lifetime: releasing the object also unregisters it
        if (mf) MFShutdown();
        if (com) CoUninitialize();
    });

    const HRESULT hr = result.get();
    if (FAILED(hr)) {
        worker_.join();
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        LOG_ERROR("Windows virtual camera: registration failed (0x%08lX: %s)", (unsigned long)hr, hrText(hr).c_str());
        return VcamStatus::Error;
    }
    lastOpenAttempt_ = 0;
    return VcamStatus::Waiting;
}

void MfVirtualCamOutput::stop() {
    closeMapping();
    if (worker_.joinable()) {
        SetEvent(stopEvent_);
        worker_.join();
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    consumer_ = false;
}

void MfVirtualCamOutput::openMapping() {
    // The media source creates the mapping once an app opens the camera
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, vcam::kMappingName);
    if (!mapping) return;
    auto* base = static_cast<uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, (SIZE_T)vcam::kMappingBytes));
    if (!base || !vcam::headerValid(base, vcam::kMappingBytes)) {
        if (base) UnmapViewOfFile(base);
        CloseHandle(mapping);
        return; // still initializing, or an incompatible DLL version
    }
    mapping_ = mapping;
    base_ = base;
    LOG_INFO("Windows virtual camera: connected to the media source");
}

void MfVirtualCamOutput::closeMapping() {
    if (base_)    { UnmapViewOfFile(base_); base_ = nullptr; }
    if (mapping_) { CloseHandle(mapping_);  mapping_ = nullptr; }
}

void MfVirtualCamOutput::send(const cv::Mat& bgr) {
    if (!isOpen() || bgr.empty()) return;
    const uint64_t now = GetTickCount64();
    if (!base_ && now - lastOpenAttempt_ >= kMappingRetryMs) {
        lastOpenAttempt_ = now;
        openMapping();
    }
    if (!base_) {
        consumer_ = false;
        return;
    }
    // Nobody is watching: skip the conversion entirely
    const bool active = vcam::consumerActive(base_, now);
    consumer_ = active;
    if (!active) return;

    uint64_t frameNo = 0;
    uint8_t* slot = vcam::beginWrite(base_, frameNo);
    nv12_.convert(bgr, (int)vcam::kWidth, (int)vcam::kHeight, slot);
    vcam::endWrite(base_, frameNo, GetTickCount64());
}

VcamStatus MfVirtualCamOutput::status() const {
    if (!isOpen()) return VcamStatus::Off;
    return consumer_ ? VcamStatus::Streaming : VcamStatus::Waiting;
}
