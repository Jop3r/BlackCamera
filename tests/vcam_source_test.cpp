// Loads BlackCameraVCam.dll in-process (no COM registration needed) and reads
// it with a standard Media Foundation source reader: media types, pacing,
// sample sizes, format switch and the "app is not running" card.
#include "vcam_shared.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <filesystem>

using Microsoft::WRL::ComPtr;

static int g_failures = 0;
#define CHECK(cond) \
    do { if (!(cond)) { std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
#define CHECK_HR(expr) \
    do { HRESULT hr_ = (expr); if (FAILED(hr_)) { std::fprintf(stderr, "FAILED %s:%d: %s = 0x%08lX\n", __FILE__, __LINE__, #expr, (unsigned long)hr_); ++g_failures; return; } } while (0)

namespace {

constexpr DWORD kStream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;

ComPtr<IMFMediaSource> activateSource(HMODULE dll) {
    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    CHECK(getClassObject != nullptr);
    if (!getClassObject) return nullptr;

    CLSID clsid{};
    CLSIDFromString(vcam::kSourceClsidString, &clsid);
    ComPtr<IClassFactory> factory;
    ComPtr<IMFActivate> activate;
    ComPtr<IMFMediaSource> source;
    if (FAILED(getClassObject(clsid, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate))) ||
        FAILED(activate->ActivateObject(IID_PPV_ARGS(&source)))) {
        CHECK(!"activation failed");
        return nullptr;
    }
    // Frame Server requirements on the source and its stream
    ComPtr<IMFMediaSourceEx> sourceEx;
    ComPtr<IMFGetService> service;
    CHECK(SUCCEEDED(source.As(&sourceEx)));
    CHECK(SUCCEEDED(source.As(&service)));
    ComPtr<IMFAttributes> streamAttrs;
    UINT32 shared = 0;
    GUID category = GUID_NULL;
    CHECK(SUCCEEDED(sourceEx->GetStreamAttributes(0, &streamAttrs)));
    if (streamAttrs) {
        CHECK(SUCCEEDED(streamAttrs->GetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, &shared)) && shared == 1);
        CHECK(SUCCEEDED(streamAttrs->GetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, &category)));
    }
    CHECK(activate->ShutdownObject() == E_NOTIMPL);
    return source;
}

void checkType(IMFSourceReader* reader, const GUID& subtype) {
    ComPtr<IMFMediaType> type;
    CHECK_HR(reader->GetCurrentMediaType(kStream, &type));
    GUID st{};
    UINT32 w = 0, h = 0, num = 0, den = 0;
    type->GetGUID(MF_MT_SUBTYPE, &st);
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
    CHECK(st == subtype);
    CHECK(w == vcam::kWidth && h == vcam::kHeight);
    CHECK(num == vcam::kFps && den == 1);
}

// Reads n samples; returns the contiguous size of the last one and its first byte
void readSamples(IMFSourceReader* reader, int n, DWORD& bytes, BYTE& first, double& seconds) {
    const auto t0 = std::chrono::steady_clock::now();
    LONGLONG lastTime = -1;
    for (int i = 0; i < n; ++i) {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        CHECK_HR(reader->ReadSample(kStream, 0, &streamIndex, &flags, &time, &sample));
        CHECK(!(flags & MF_SOURCE_READERF_ERROR));
        if (!sample) { --i; continue; } // stream tick without data
        CHECK(time > lastTime);
        lastTime = time;
        ComPtr<IMFMediaBuffer> buffer;
        CHECK_HR(sample->ConvertToContiguousBuffer(&buffer));
        BYTE* data = nullptr;
        CHECK_HR(buffer->Lock(&data, nullptr, &bytes));
        first = data[0];
        buffer->Unlock();
    }
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void testSource(HMODULE dll) {
    ComPtr<IMFMediaSource> source = activateSource(dll);
    if (!source) return;

    ComPtr<IMFSourceReader> reader;
    CHECK_HR(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader));
    checkType(reader.Get(), MFVideoFormat_NV12);

    // No BlackCamera.exe producing frames here: the placeholder card (black, Y=16)
    DWORD bytes = 0;
    BYTE first = 0;
    double seconds = 0;
    readSamples(reader.Get(), 31, bytes, first, seconds);
    std::printf("NV12: 31 samples in %.2f s, %lu bytes each\n", seconds, (unsigned long)bytes);
    CHECK(bytes == vcam::kFrameBytes);
    CHECK(first == 16);
    CHECK(seconds > 0.8 && seconds < 1.6); // paced at ~30 FPS, not as fast as possible

    // Second native type
    ComPtr<IMFMediaType> yuy2;
    CHECK_HR(reader->GetNativeMediaType(kStream, 1, &yuy2));
    CHECK_HR(reader->SetCurrentMediaType(kStream, nullptr, yuy2.Get()));
    checkType(reader.Get(), MFVideoFormat_YUY2);
    readSamples(reader.Get(), 3, bytes, first, seconds);
    CHECK(bytes == vcam::kWidth * vcam::kHeight * 2);
    CHECK(first == 16);

    reader.Reset();
    source->Shutdown(); // the reader may already have done it
}

} // namespace

int main() {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    CHECK(SUCCEEDED(MFStartup(MF_VERSION)));

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto dllPath = std::filesystem::path(exe).parent_path() / L"BlackCameraVCam.dll";
    HMODULE dll = LoadLibraryW(dllPath.c_str());
    CHECK(dll != nullptr);
    if (dll) testSource(dll);

    MFShutdown();
    if (com) CoUninitialize();
    if (g_failures == 0) std::puts("vcam_source_test: all checks passed");
    return g_failures == 0 ? 0 : 1;
}
