// Tests for the app <-> BlackCameraVCam.dll shared-memory protocol (vcam_shared.h)
// and the BGR -> NV12 conversion used by both virtual camera outputs.
#include "nv12.h"
#include "vcam_shared.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) \
    do { if (!(cond)) { std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

struct Mapping {
    std::vector<uint64_t> words = std::vector<uint64_t>(vcam::kMappingBytes / sizeof(uint64_t), 0);
    void* base() { return words.data(); }
    vcam::SharedHeader* header() { return static_cast<vcam::SharedHeader*>(base()); }
};

void writeFrame(void* base, uint8_t value, uint64_t tick) {
    uint64_t frameNo = 0;
    uint8_t* slot = vcam::beginWrite(base, frameNo);
    std::fill(slot, slot + vcam::kFrameBytes, value);
    vcam::endWrite(base, frameNo, tick);
}

bool uniform(const std::vector<uint8_t>& v, uint8_t value) {
    return std::all_of(v.begin(), v.end(), [value](uint8_t b) { return b == value; });
}

void testProtocol() {
    Mapping m;
    std::vector<uint8_t> frame(vcam::kFrameBytes);

    // Header handshake
    CHECK(!vcam::headerValid(m.base(), vcam::kMappingBytes));
    vcam::initHeader(m.base());
    CHECK(vcam::headerValid(m.base(), vcam::kMappingBytes));
    CHECK(!vcam::headerValid(m.base(), vcam::kMappingBytes - 1));
    CHECK(vcam::kHeaderBytes >= sizeof(vcam::SharedHeader));
    CHECK(m.header()->slotOffset[vcam::kSlotCount - 1] + vcam::kFrameBytes <= vcam::kMappingBytes);

    // No producer yet; reading still marks the consumer as alive
    const uint64_t t0 = 100'000;
    CHECK(!vcam::consumerActive(m.base(), t0));
    CHECK(vcam::readLatest(m.base(), frame.data(), t0) == vcam::ReadResult::NoProducer);
    CHECK(vcam::consumerActive(m.base(), t0 + 10));
    CHECK(!vcam::consumerActive(m.base(), t0 + vcam::kConsumerTimeoutMs + 1));

    // One frame round trip
    writeFrame(m.base(), 7, t0);
    uint64_t frameNo = 0;
    CHECK(vcam::readLatest(m.base(), frame.data(), t0 + 5, &frameNo) == vcam::ReadResult::Frame);
    CHECK(frameNo == 1);
    CHECK(uniform(frame, 7));

    // The newest of several frames wins, slots rotate
    writeFrame(m.base(), 8, t0 + 33);
    writeFrame(m.base(), 9, t0 + 66);
    writeFrame(m.base(), 10, t0 + 99);
    CHECK(vcam::readLatest(m.base(), frame.data(), t0 + 100, &frameNo) == vcam::ReadResult::Frame);
    CHECK(frameNo == 4);
    CHECK(uniform(frame, 10));

    // A producer tick slightly "ahead" of the consumer clock still counts as alive
    CHECK(vcam::readLatest(m.base(), frame.data(), t0 + 50) == vcam::ReadResult::Frame);

    // Producer gone
    CHECK(vcam::readLatest(m.base(), frame.data(), t0 + 99 + vcam::kProducerTimeoutMs + 1) == vcam::ReadResult::NoProducer);

    // A header tampered with after validation must not redirect reads or writes
    for (auto& off : m.header()->slotOffset) off = ~0ull;
    writeFrame(m.base(), 12, t0 + 150);
    CHECK(vcam::readLatest(m.base(), frame.data(), t0 + 151) == vcam::ReadResult::Frame);
    CHECK(uniform(frame, 12));

    // Slot being rewritten -> torn
    writeFrame(m.base(), 11, t0 + 200);
    const uint64_t latest = m.header()->latestFrame;
    m.header()->slotFrame[latest % vcam::kSlotCount] = 0;
    CHECK(vcam::readLatest(m.base(), frame.data(), t0 + 201) == vcam::ReadResult::Torn);
}

// A real writer thread against a real reader thread: every frame the reader
// accepts must be one complete frame, never a mix of two.
void testConcurrent() {
    Mapping m;
    vcam::initHeader(m.base());
    std::atomic<bool> done{ false };

    std::thread writer([&] {
        for (int i = 1; i <= 300; ++i) writeFrame(m.base(), (uint8_t)i, 1'000 + i);
        done = true;
    });

    std::vector<uint8_t> frame(vcam::kFrameBytes);
    int frames = 0, mixed = 0;
    while (!done) {
        if (vcam::readLatest(m.base(), frame.data(), 1'000 + 300) != vcam::ReadResult::Frame) continue;
        ++frames;
        // Compare a few spread-out bytes instead of all 3 MB to keep the race window realistic
        const uint8_t v = frame[0];
        if (frame[vcam::kFrameBytes / 2] != v || frame[vcam::kFrameBytes - 1] != v) ++mixed;
    }
    writer.join();
    std::printf("concurrent: %d frames read, %d mixed\n", frames, mixed);
    CHECK(mixed == 0);
}

void testNv12() {
    Nv12Converter conv;
    std::vector<uint8_t> out(8 * 4 * 3 / 2);

    // Pure green at the output size: BT.601 limited range Y=145, U=54, V=34
    cv::Mat green(4, 8, CV_8UC3, cv::Scalar(0, 255, 0));
    conv.convert(green, 8, 4, out.data());
    CHECK(std::abs(out[0] - 145) <= 2);
    CHECK(std::abs(out[8 * 4] - 54) <= 2);     // first U
    CHECK(std::abs(out[8 * 4 + 1] - 34) <= 2); // first V

    // Different input size is scaled; black -> Y=16, U=V=128
    cv::Mat black(10, 20, CV_8UC3, cv::Scalar(0, 0, 0));
    conv.convert(black, 8, 4, out.data());
    CHECK(std::all_of(out.begin(), out.begin() + 32, [](uint8_t y) { return std::abs(y - 16) <= 1; }));
    CHECK(std::all_of(out.begin() + 32, out.end(), [](uint8_t c) { return std::abs(c - 128) <= 1; }));
}

} // namespace

int main() {
    testProtocol();
    testConcurrent();
    testNv12();
    if (g_failures == 0) std::puts("vcam_protocol_test: all checks passed");
    return g_failures == 0 ? 0 : 1;
}
