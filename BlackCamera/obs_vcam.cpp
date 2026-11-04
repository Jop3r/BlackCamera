#include "obs_vcam.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iterator>

#pragma comment(lib, "advapi32.lib") // RegOpenKeyExW

namespace {

constexpr const wchar_t* kMappingName = L"OBSVirtualCamVideo";
// CLSID under which OBS registers its virtual camera DirectShow filter
constexpr const wchar_t* kFilterKey = L"CLSID\\{A3FCE0F5-3493-419F-958A-ABA1250EC20B}";

constexpr uint32_t kAlign      = 32;
constexpr uint32_t kSlotPrefix = 32;

enum : uint32_t { StateStarting = 1, StateReady = 2, StateStopping = 3 };

constexpr uint32_t alignUp(uint32_t v) { return (v + kAlign - 1) & ~(kAlign - 1); }

} // namespace

bool ObsVirtualCamOutput::isFilterInstalled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, kFilterKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

VcamStatus ObsVirtualCamOutput::start(int width, int height, double fps) {
    stop();
    if (!isFilterInstalled()) return VcamStatus::NotInstalled;

    // Somebody (usually OBS itself) already produces frames into the block
    if (HANDLE existing = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName)) {
        CloseHandle(existing);
        return VcamStatus::Busy;
    }

    w_ = width & ~1;   // NV12 needs even dimensions
    h_ = height & ~1;
    if (w_ <= 0 || h_ <= 0) return VcamStatus::Error;

    const uint32_t imageBytes = (uint32_t)w_ * (uint32_t)h_ * 3 / 2;
    uint32_t offsets[kSlotCount];
    uint32_t total = alignUp(sizeof(ControlBlock));
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        offsets[i] = total;
        total = alignUp(total + kSlotPrefix + imageBytes);
    }

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, total, kMappingName);
    if (!mapping) return VcamStatus::Error;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { // lost a race with another producer
        CloseHandle(mapping);
        return VcamStatus::Busy;
    }

    base_ = static_cast<uint8_t*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, total));
    if (!base_) {
        CloseHandle(mapping);
        return VcamStatus::Error;
    }
    mapping_ = mapping;

    // A freshly created mapping is zero-filled by the OS
    ctrl_ = reinterpret_cast<ControlBlock*>(base_);
    ctrl_->format        = 0;
    ctrl_->width         = (uint32_t)w_;
    ctrl_->height        = (uint32_t)h_;
    ctrl_->frameInterval = (uint64_t)(10'000'000.0 / fps);
    std::copy(std::begin(offsets), std::end(offsets), ctrl_->slotOffset);
    std::atomic_ref<uint32_t>(ctrl_->state).store(StateStarting, std::memory_order_release);
    return VcamStatus::Streaming;
}

void ObsVirtualCamOutput::send(const cv::Mat& bgr) {
    if (!ctrl_ || bgr.empty()) return;

    const uint32_t frameNo = std::atomic_ref<uint32_t>(ctrl_->framesWritten).fetch_add(1) + 1;
    uint8_t* slot = base_ + ctrl_->slotOffset[frameNo % kSlotCount];

    const uint64_t ts = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::memcpy(slot, &ts, sizeof(ts));

    nv12_.convert(bgr, w_, h_, slot + kSlotPrefix);

    std::atomic_ref<uint32_t>(ctrl_->latestFrame).store(frameNo, std::memory_order_release);
    std::atomic_ref<uint32_t>(ctrl_->state).store(StateReady, std::memory_order_release);
}

void ObsVirtualCamOutput::stop() {
    if (ctrl_) {
        std::atomic_ref<uint32_t>(ctrl_->state).store(StateStopping, std::memory_order_release);
        ctrl_ = nullptr;
    }
    if (base_)    { UnmapViewOfFile(base_); base_ = nullptr; }
    if (mapping_) { CloseHandle(mapping_);  mapping_ = nullptr; }
}
