#pragma once
// Consumer side of the shared-memory frame exchange (see BlackCamera/vcam_shared.h).

#include <cstdint>
#include <vector>

namespace vcamdll {

class SharedFrameReader {
public:
    SharedFrameReader();
    ~SharedFrameReader() { close(); }
    SharedFrameReader(const SharedFrameReader&) = delete;
    SharedFrameReader& operator=(const SharedFrameReader&) = delete;

    // Fills dst with one NV12 frame (vcam::kFrameBytes, tightly packed): the
    // newest frame from BlackCamera.exe, or a "not running" card when the app
    // is not producing. Returns true for a live frame.
    bool read(uint8_t* dst);
    void close();

private:
    bool open();

    void*    mapping_ = nullptr; // HANDLE
    uint8_t* base_ = nullptr;
    uint64_t lastOpenAttempt_ = 0;
    uint64_t lastFrameNo_ = 0;
    std::vector<uint8_t> lastFrame_;   // repeated on a torn read
    std::vector<uint8_t> placeholder_; // shown while the app is not running
};

// Draws the "BlackCamera is not running" card (NV12, vcam::kWidth x vcam::kHeight)
void drawPlaceholder(uint8_t* nv12);

} // namespace vcamdll
