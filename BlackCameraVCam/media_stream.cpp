#include "media_stream.h"
#include "vcam_shared.h"

#include <propvarutil.h>

#include <chrono>
#include <cstring>

namespace vcamdll {

namespace {

using Clock = std::chrono::steady_clock;
constexpr LONGLONG kFrameDuration = 10'000'000LL / vcam::kFps; // 100 ns units
constexpr auto kFrameInterval = std::chrono::nanoseconds(1'000'000'000LL / vcam::kFps);

HRESULT makeVideoType(const GUID& subtype, UINT32 stride, UINT32 sampleSize, IMFMediaType** out) {
    ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return hr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, vcam::kWidth, vcam::kHeight);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, vcam::kFps, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    type->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
    type->SetUINT32(MF_MT_SAMPLE_SIZE, sampleSize);
    type->SetUINT32(MF_MT_AVG_BITRATE, sampleSize * 8 * vcam::kFps);
    *out = type.Detach();
    return S_OK;
}

// NV12 (tight) -> NV12 in a buffer with its own pitch
void copyNv12(const uint8_t* src, uint8_t* dst, LONG pitch) {
    constexpr int w = (int)vcam::kWidth, h = (int)vcam::kHeight;
    for (int y = 0; y < h; ++y) std::memcpy(dst + (ptrdiff_t)y * pitch, src + (size_t)y * w, w);
    const uint8_t* srcUv = src + (size_t)w * h;
    uint8_t* dstUv = dst + (ptrdiff_t)pitch * h;
    for (int y = 0; y < h / 2; ++y) std::memcpy(dstUv + (ptrdiff_t)y * pitch, srcUv + (size_t)y * w, w);
}

// NV12 (tight) -> YUY2 (Y0 U Y1 V), for DirectShow-era apps that expect it
void copyYuy2(const uint8_t* src, uint8_t* dst, LONG pitch) {
    constexpr int w = (int)vcam::kWidth, h = (int)vcam::kHeight;
    const uint8_t* srcUv = src + (size_t)w * h;
    for (int y = 0; y < h; ++y) {
        const uint8_t* luma = src + (size_t)y * w;
        const uint8_t* uv = srcUv + (size_t)(y / 2) * w;
        uint8_t* out = dst + (ptrdiff_t)y * pitch;
        for (int x = 0; x < w; x += 2) {
            out[0] = luma[x];
            out[1] = uv[x];
            out[2] = luma[x + 1];
            out[3] = uv[x + 1];
            out += 4;
        }
    }
}

} // namespace

HRESULT MediaStream::create(IMFMediaSource* source, DWORD streamId, MediaStream** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    auto* stream = new (std::nothrow) MediaStream();
    if (!stream) return E_OUTOFMEMORY;
    const HRESULT hr = stream->init(source, streamId);
    if (FAILED(hr)) {
        stream->Release();
        return hr;
    }
    *out = stream;
    return S_OK;
}

HRESULT MediaStream::init(IMFMediaSource* source, DWORD streamId) {
    source_ = source;
    frame_.resize(vcam::kFrameBytes);

    HRESULT hr = MFCreateAttributes(&attributes_, 4);
    if (FAILED(hr)) return hr;
    // Required by Frame Server for custom media source streams
    attributes_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    attributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, streamId);
    // Recommended: shareable by several apps at once, plain color stream
    attributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    attributes_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

    if (FAILED(hr = MFCreateEventQueue(&queue_))) return hr;

    ComPtr<IMFMediaType> nv12, yuy2;
    if (FAILED(hr = makeVideoType(MFVideoFormat_NV12, vcam::kWidth, vcam::kFrameBytes, &nv12))) return hr;
    if (FAILED(hr = makeVideoType(MFVideoFormat_YUY2, vcam::kWidth * 2, vcam::kWidth * vcam::kHeight * 2, &yuy2))) return hr;
    IMFMediaType* types[] = { nv12.Get(), yuy2.Get() };
    if (FAILED(hr = MFCreateStreamDescriptor(streamId, 2, types, &descriptor_))) return hr;

    ComPtr<IMFMediaTypeHandler> handler;
    if (FAILED(hr = descriptor_->GetMediaTypeHandler(&handler))) return hr;
    if (FAILED(hr = handler->SetCurrentMediaType(nv12.Get()))) return hr;
    subtype_ = MFVideoFormat_NV12;

    worker_ = std::thread(&MediaStream::deliveryLoop, this);
    return S_OK;
}

STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) ||
        riid == __uuidof(IMFMediaStream) || riid == __uuidof(IMFMediaStream2)) {
        *ppv = static_cast<IMFMediaStream2*>(this);
    } else if (riid == __uuidof(IKsControl)) {
        *ppv = static_cast<IKsControl*>(this);
    } else if (riid == __uuidof(IMFAttributes)) {
        *ppv = static_cast<IMFAttributes*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

// ── Control (called by the media source) ────────────────────────────────────

HRESULT MediaStream::start(IMFMediaType* type) {
    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;
    if (type) {
        GUID subtype = GUID_NULL;
        if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_YUY2))
            subtype_ = subtype;
    }
    state_ = MF_STREAM_STATE_RUNNING;
    trace(L"BlackCameraVCam: stream started (%s)", subtype_ == MFVideoFormat_YUY2 ? L"YUY2" : L"NV12");
    PROPVARIANT now;
    InitPropVariantFromInt64(MFGetSystemTime(), &now);
    const HRESULT hr = queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, &now);
    wake_.notify_all();
    return hr;
}

HRESULT MediaStream::stop() {
    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;
    state_ = MF_STREAM_STATE_STOPPED;
    pending_.clear();
    trace(L"BlackCameraVCam: stream stopped");
    return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void MediaStream::shutdown() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
        state_ = MF_STREAM_STATE_STOPPED;
        pending_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();

    std::lock_guard lock(mutex_);
    if (queue_) {
        queue_->Shutdown();
        queue_.Reset();
    }
    source_.Reset();
    reader_.close();
}

// ── Sample delivery ─────────────────────────────────────────────────────────

void MediaStream::deliveryLoop() {
    auto next = Clock::now();
    std::unique_lock lock(mutex_);
    while (!quit_) {
        if (state_ != MF_STREAM_STATE_RUNNING || pending_.empty()) {
            wake_.wait(lock, [&] { return quit_ || (state_ == MF_STREAM_STATE_RUNNING && !pending_.empty()); });
            continue;
        }
        // Pace delivery to the camera frame rate
        if (Clock::now() < next) {
            wake_.wait_until(lock, next, [&] { return quit_; });
            continue;
        }
        next += kFrameInterval;
        if (Clock::now() > next + kFrameInterval) next = Clock::now() + kFrameInterval; // fell behind: resync

        ComPtr<IUnknown> token = std::move(pending_.front());
        pending_.pop_front();
        const GUID subtype = subtype_;

        lock.unlock();
        ComPtr<IMFSample> sample;
        HRESULT hr = buildSample(subtype, &sample);
        if (SUCCEEDED(hr) && token) hr = sample->SetUnknown(MFSampleExtension_Token, token.Get());
        lock.lock();

        if (FAILED(hr)) {
            trace(L"BlackCameraVCam: building a sample failed (0x%08X)", hr);
            if (queue_) queue_->QueueEventParamVar(MEError, GUID_NULL, hr, nullptr);
            continue;
        }
        if (state_ == MF_STREAM_STATE_RUNNING && queue_)
            queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    }
}

HRESULT MediaStream::buildSample(const GUID& subtype, IMFSample** out) {
    reader_.read(frame_.data()); // live frame or the "not running" card

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = MFCreate2DMediaBuffer(vcam::kWidth, vcam::kHeight, subtype.Data1, FALSE, &buffer))) return hr;

    ComPtr<IMF2DBuffer2> buffer2d;
    if (FAILED(hr = buffer.As(&buffer2d))) return hr;
    BYTE* scan0 = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    if (FAILED(hr = buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &length))) return hr;
    if (subtype == MFVideoFormat_YUY2) copyYuy2(frame_.data(), scan0, pitch);
    else copyNv12(frame_.data(), scan0, pitch);
    buffer2d->Unlock2D();

    if (FAILED(hr = sample->AddBuffer(buffer.Get()))) return hr;
    // Frame Server expects QPC-based timestamps in 100 ns units
    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration(kFrameDuration);
    *out = sample.Detach();
    return S_OK;
}

// ── IMFMediaStream ──────────────────────────────────────────────────────────

STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;
    if (state_ == MF_STREAM_STATE_STOPPED) return MF_E_MEDIA_SOURCE_WRONGSTATE;
    pending_.emplace_back(token);
    wake_.notify_all();
    return S_OK;
}

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    std::lock_guard lock(mutex_);
    *source = nullptr;
    if (!source_) return MF_E_SHUTDOWN;
    return source_.CopyTo(source);
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    std::lock_guard lock(mutex_);
    *descriptor = nullptr;
    if (!queue_) return MF_E_SHUTDOWN;
    return descriptor_.CopyTo(descriptor);
}

// ── IMFMediaStream2 ─────────────────────────────────────────────────────────

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    MF_STREAM_STATE current;
    {
        std::lock_guard lock(mutex_);
        if (!queue_) return MF_E_SHUTDOWN;
        current = state_;
    }
    if (state == current) return S_OK;
    switch (state) {
        case MF_STREAM_STATE_RUNNING: return start(nullptr);
        case MF_STREAM_STATE_STOPPED: return stop();
        case MF_STREAM_STATE_PAUSED: {
            std::lock_guard lock(mutex_);
            if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;
            state_ = MF_STREAM_STATE_PAUSED;
            return S_OK;
        }
        default: return MF_E_INVALID_STATE_TRANSITION;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    std::lock_guard lock(mutex_);
    *state = state_;
    return S_OK;
}

// ── IMFMediaEventGenerator ──────────────────────────────────────────────────

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(result, event);
}

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    // May block: do not hold the lock while waiting
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard lock(mutex_);
        if (!queue_) return MF_E_SHUTDOWN;
        queue = queue_;
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID extType, HRESULT status, const PROPVARIANT* value) {
    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(type, extType, status, value);
}

} // namespace vcamdll
