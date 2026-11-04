#include "frame_reader.h"
#include "common.h"
#include "vcam_shared.h"

#include <sddl.h>

#include <cstring>
#include <string_view>

namespace vcamdll {

namespace {

constexpr uint64_t kReopenIntervalMs = 1000;

// 5x7 bitmap font, only the letters the placeholder needs; bit 4 = leftmost column
struct Glyph { char c; uint8_t rows[7]; };
constexpr Glyph kFont[] = {
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
};

const uint8_t* glyphRows(char c) {
    for (const Glyph& g : kFont)
        if (g.c == c) return g.rows;
    return nullptr; // space and anything unknown
}

// Draws text centered horizontally, `scale` pixels per font dot, in green
void drawText(uint8_t* nv12, std::string_view text, int top, int scale) {
    constexpr int w = (int)vcam::kWidth, h = (int)vcam::kHeight;
    constexpr uint8_t kY = 145, kU = 54, kV = 34; // RGB (0, 255, 0), BT.601 limited range
    uint8_t* luma = nv12;
    uint8_t* chroma = nv12 + (size_t)w * h;

    const int advance = 6 * scale;
    int x0 = (w - (int)text.size() * advance + scale) / 2;
    for (char c : text) {
        if (const uint8_t* rows = glyphRows(c)) {
            for (int r = 0; r < 7; ++r) {
                for (int col = 0; col < 5; ++col) {
                    if (!(rows[r] & (0x10 >> col))) continue;
                    for (int dy = 0; dy < scale; ++dy) {
                        const int y = top + r * scale + dy;
                        if (y < 0 || y >= h) continue;
                        for (int dx = 0; dx < scale; ++dx) {
                            const int x = x0 + col * scale + dx;
                            if (x < 0 || x >= w) continue;
                            luma[(size_t)y * w + x] = kY;
                            uint8_t* uv = chroma + (size_t)(y / 2) * w + (x & ~1);
                            uv[0] = kU;
                            uv[1] = kV;
                        }
                    }
                }
            }
        }
        x0 += advance;
    }
}

} // namespace

void drawPlaceholder(uint8_t* nv12) {
    const size_t luma = (size_t)vcam::kWidth * vcam::kHeight;
    std::memset(nv12, 16, luma);              // black
    std::memset(nv12 + luma, 128, luma / 2);  // neutral chroma
    drawText(nv12, "BLACKCAMERA", 400, 16);
    drawText(nv12, "APP IS NOT RUNNING", 580, 6);
}

SharedFrameReader::SharedFrameReader() {
    placeholder_.resize(vcam::kFrameBytes);
    drawPlaceholder(placeholder_.data());
}

bool SharedFrameReader::open() {
    // The Frame Server (LOCAL SERVICE) may create Global\ objects; client
    // processes may not, they can only open an existing mapping.
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(vcam::kMappingSddl, SDDL_REVISION_1, &sd, nullptr))
        sa.lpSecurityDescriptor = sd;

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                        (DWORD)(vcam::kMappingBytes >> 32), (DWORD)vcam::kMappingBytes,
                                        vcam::kMappingName);
    const DWORD createError = GetLastError();
    if (sd) LocalFree(sd);

    bool created = mapping && createError != ERROR_ALREADY_EXISTS;
    if (!mapping) {
        mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, vcam::kMappingName);
        if (!mapping) {
            trace(L"BlackCameraVCam: shared memory unavailable (create error %lu, open error %lu)", createError, GetLastError());
            return false;
        }
        created = false;
    }

    auto* base = static_cast<uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, (SIZE_T)vcam::kMappingBytes));
    if (!base) {
        trace(L"BlackCameraVCam: MapViewOfFile failed (%lu)", GetLastError());
        CloseHandle(mapping);
        return false;
    }
    if (created) vcam::initHeader(base);
    if (!vcam::headerValid(base, vcam::kMappingBytes)) {
        // Another (incompatible or still initializing) creator; retry later
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        return false;
    }
    mapping_ = mapping;
    base_ = base;
    trace(L"BlackCameraVCam: shared memory %s", created ? L"created" : L"opened");
    return true;
}

void SharedFrameReader::close() {
    if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
    if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
}

bool SharedFrameReader::read(uint8_t* dst) {
    const uint64_t now = GetTickCount64();
    if (!base_ && now - lastOpenAttempt_ >= kReopenIntervalMs) {
        lastOpenAttempt_ = now;
        open();
    }
    if (base_) {
        uint64_t frameNo = 0;
        switch (vcam::readLatest(base_, dst, now, &frameNo)) {
            case vcam::ReadResult::Frame:
                if (frameNo != lastFrameNo_) {
                    lastFrameNo_ = frameNo;
                    lastFrame_.assign(dst, dst + vcam::kFrameBytes);
                }
                return true;
            case vcam::ReadResult::Torn:
                if (!lastFrame_.empty()) {
                    std::memcpy(dst, lastFrame_.data(), vcam::kFrameBytes);
                    return true;
                }
                break;
            case vcam::ReadResult::NoProducer:
                lastFrame_.clear();
                lastFrameNo_ = 0;
                break;
        }
    }
    std::memcpy(dst, placeholder_.data(), vcam::kFrameBytes);
    return false;
}

} // namespace vcamdll
