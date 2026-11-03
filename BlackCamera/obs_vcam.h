#pragma once
// OBS Virtual Camera output.
//
// The OBS Virtual Camera DirectShow filter (installed with OBS Studio) reads
// frames from a named shared-memory block. We act as the producer, the same
// role OBS itself takes when "Start Virtual Camera" is pressed, so only one of
// the two can own the block at a time.
//
// Block layout:
//   ControlBlock (80 B, padded to 32 B)
//   3 x slot, each starting on a 32 B boundary:
//       32 B slot prefix (first 8 B = timestamp, ns)
//       NV12 image: Y plane (w*h) + interleaved UV plane (w*h/2)
//
// The producer bumps `framesWritten`, fills slot (framesWritten % 3), then
// publishes it through `latestFrame` and switches `state` to Ready.

#include <opencv2/core.hpp>

#include <cstddef>
#include <cstdint>

enum class VcamStatus : int { Off, NotInstalled, Busy, Error, Streaming };

class ObsVirtualCamOutput {
public:
    static bool isFilterInstalled();

    VcamStatus start(int width, int height, double fps);
    void send(const cv::Mat& bgr);
    void stop();
    bool isOpen() const { return ctrl_ != nullptr; }

    ObsVirtualCamOutput() = default;
    ObsVirtualCamOutput(const ObsVirtualCamOutput&) = delete;
    ObsVirtualCamOutput& operator=(const ObsVirtualCamOutput&) = delete;
    ~ObsVirtualCamOutput() { stop(); }

private:
    static constexpr uint32_t kSlotCount = 3;

    struct ControlBlock {
        uint32_t framesWritten;
        uint32_t latestFrame;
        uint32_t state;
        uint32_t slotOffset[kSlotCount];
        uint32_t format;              // 0 = video
        uint32_t width;
        uint32_t height;
        uint64_t frameInterval;       // 100 ns units (DirectShow REFERENCE_TIME)
        uint32_t reserved[8];
    };
    static_assert(offsetof(ControlBlock, frameInterval) == 40, "ControlBlock layout mismatch");
    static_assert(sizeof(ControlBlock) == 80, "ControlBlock layout mismatch");

    void*         mapping_ = nullptr; // HANDLE
    uint8_t*      base_    = nullptr;
    ControlBlock* ctrl_    = nullptr;
    int           w_ = 0, h_ = 0;
    cv::Mat       scaled_, i420_;
};
