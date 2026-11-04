#pragma once
// Frame exchange between BlackCamera.exe (producer) and BlackCameraVCam.dll, the
// Media Foundation virtual camera source (consumer).
//
// The DLL is loaded by the Windows Camera Frame Server service, which runs as
// LOCAL SERVICE in session 0, while the app runs in the user's session. They
// share a named file mapping in the Global\ namespace. Creating Global\ objects
// needs SeCreateGlobalPrivilege, which LOCAL SERVICE has and a normal user does
// not, so the DLL creates the mapping (with a DACL that lets interactive users
// write to it) and the app only opens it.
//
// Synchronization is lock-free on purpose: a mutex abandoned by a crashed app
// must never stall the camera service. The producer fills one of three slots
// and publishes its frame number; the consumer copies the newest slot and
// re-checks the slot's frame number afterwards to detect a torn read.
//
// Layout (all offsets from the mapping base, slots 4 KiB aligned):
//   SharedHeader
//   kSlotCount x NV12 frame (Y plane w*h, interleaved UV plane w*h/2)
//
// Header-only and free of OpenCV/MF so both binaries and the tests can use it.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vcam {

// CLSID of the media source COM class in BlackCameraVCam.dll
inline constexpr wchar_t kSourceClsidString[] = L"{5626581E-9D19-4D71-80DA-304C9EA258D9}";
inline constexpr wchar_t kMappingName[]       = L"Global\\BlackCameraVirtualCameraFrames";
// Windows appends "Windows Virtual Camera" to this name in device lists
inline constexpr wchar_t kFriendlyName[]      = L"BlackCamera";

// Interactive users read/write; the camera services and administrators full access
inline constexpr wchar_t kMappingSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGW;;;IU)";

constexpr uint32_t kMagic     = 0x43564342; // "BCVC"
constexpr uint32_t kVersion   = 1;
constexpr uint32_t kWidth     = 1920;
constexpr uint32_t kHeight    = 1080;
constexpr uint32_t kFps       = 30;
constexpr uint32_t kSlotCount = 3;
constexpr uint32_t kFrameBytes = kWidth * kHeight * 3 / 2; // NV12

// A producer that has not written for this long is treated as gone
constexpr uint64_t kProducerTimeoutMs = 1500;
// A consumer that has not read for this long is treated as gone
constexpr uint64_t kConsumerTimeoutMs = 2000;

constexpr uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

struct SharedHeader {
    uint32_t magic;          // written last by the creator: kMagic once the header is valid
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t frameBytes;
    uint32_t slotCount;
    uint32_t reserved0;
    uint64_t slotOffset[kSlotCount];
    uint64_t latestFrame;            // number of the newest complete frame, 0 = none yet
    uint64_t slotFrame[kSlotCount];  // frame number a slot holds, 0 while it is being written
    uint64_t producerTick;           // GetTickCount64() of the last frame written by the app
    uint64_t consumerTick;           // GetTickCount64() of the last frame read by the media source
    uint64_t reserved[8];
};

constexpr uint64_t kHeaderBytes = alignUp(sizeof(SharedHeader), 4096);
constexpr uint64_t kSlotStride  = alignUp(kFrameBytes, 4096);
constexpr uint64_t kMappingBytes = kHeaderBytes + kSlotStride * kSlotCount;

namespace detail {
// Slot addresses always come from constants, never from the (writable by
// other processes) header, so a tampered header cannot redirect a copy
// outside the mapping. slotOffset[] is informational and only checked once.
inline uint8_t* slotAddress(void* base, uint64_t frameNo) {
    return static_cast<uint8_t*>(base) + kHeaderBytes + kSlotStride * (frameNo % kSlotCount);
}
// Ticks come from different processes, so "later" may read slightly smaller
inline uint64_t elapsed(uint64_t now, uint64_t then) { return now > then ? now - then : 0; }
inline uint64_t load(const uint64_t& v) {
    return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(v)).load(std::memory_order_acquire);
}
inline void store(uint64_t& v, uint64_t x) {
    std::atomic_ref<uint64_t>(v).store(x, std::memory_order_release);
}
} // namespace detail

// Creator side: fills a fresh, zeroed mapping
inline void initHeader(void* base) {
    auto* h = static_cast<SharedHeader*>(base);
    h->version    = kVersion;
    h->width      = kWidth;
    h->height     = kHeight;
    h->fps        = kFps;
    h->frameBytes = kFrameBytes;
    h->slotCount  = kSlotCount;
    for (uint32_t i = 0; i < kSlotCount; ++i) h->slotOffset[i] = kHeaderBytes + kSlotStride * i;
    std::atomic_ref<uint32_t>(h->magic).store(kMagic, std::memory_order_release);
}

// True when the mapping was set up by a compatible creator
inline bool headerValid(const void* base, uint64_t mappedBytes) {
    if (!base || mappedBytes < kMappingBytes) return false;
    const auto* h = static_cast<const SharedHeader*>(base);
    if (std::atomic_ref<uint32_t>(const_cast<uint32_t&>(h->magic)).load(std::memory_order_acquire) != kMagic) return false;
    if (h->version != kVersion || h->width != kWidth || h->height != kHeight ||
        h->frameBytes != kFrameBytes || h->slotCount != kSlotCount) return false;
    for (uint32_t i = 0; i < kSlotCount; ++i)
        if (h->slotOffset[i] != kHeaderBytes + kSlotStride * i) return false;
    return true;
}

// ── Producer (app) ──────────────────────────────────────────────────────────

// Returns the slot to fill with the next NV12 frame; call endWrite() afterwards
inline uint8_t* beginWrite(void* base, uint64_t& frameNo) {
    auto* h = static_cast<SharedHeader*>(base);
    frameNo = detail::load(h->latestFrame) + 1;
    const uint32_t slot = (uint32_t)(frameNo % kSlotCount);
    detail::store(h->slotFrame[slot], 0); // invalidate before touching the pixels
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return detail::slotAddress(base, frameNo);
}

inline void endWrite(void* base, uint64_t frameNo, uint64_t nowTick) {
    auto* h = static_cast<SharedHeader*>(base);
    const uint32_t slot = (uint32_t)(frameNo % kSlotCount);
    detail::store(h->slotFrame[slot], frameNo);
    detail::store(h->latestFrame, frameNo);
    detail::store(h->producerTick, nowTick);
}

// True while a media source instance is pulling frames
inline bool consumerActive(const void* base, uint64_t nowTick) {
    const auto* h = static_cast<const SharedHeader*>(base);
    const uint64_t t = detail::load(h->consumerTick);
    return t != 0 && detail::elapsed(nowTick, t) < kConsumerTimeoutMs;
}

// ── Consumer (media source) ─────────────────────────────────────────────────

enum class ReadResult { Frame, NoProducer, Torn };

// Copies the newest frame (kFrameBytes, NV12, tightly packed) into dst.
// Also records that a consumer is alive, so the app keeps producing.
inline ReadResult readLatest(void* base, uint8_t* dst, uint64_t nowTick, uint64_t* frameNoOut = nullptr) {
    auto* h = static_cast<SharedHeader*>(base);
    detail::store(h->consumerTick, nowTick);

    const uint64_t produced = detail::load(h->producerTick);
    if (produced == 0 || detail::elapsed(nowTick, produced) > kProducerTimeoutMs) return ReadResult::NoProducer;

    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t frameNo = detail::load(h->latestFrame);
        if (frameNo == 0) return ReadResult::NoProducer;
        const uint32_t slot = (uint32_t)(frameNo % kSlotCount);
        if (detail::load(h->slotFrame[slot]) != frameNo) continue; // producer already reusing it
        std::memcpy(dst, detail::slotAddress(base, frameNo), kFrameBytes);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (detail::load(h->slotFrame[slot]) == frameNo) {
            if (frameNoOut) *frameNoOut = frameNo;
            return ReadResult::Frame;
        }
    }
    return ReadResult::Torn;
}

} // namespace vcam
