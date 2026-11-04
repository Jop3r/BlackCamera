#include "media_source.h"

#include <propvarutil.h>

namespace vcamdll {

namespace {
constexpr DWORD kStreamId = 0; // Frame Server wants 0-based stream ids
}

HRESULT MediaSource::create(IMFAttributes* initial, MediaSource** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    auto* source = new (std::nothrow) MediaSource();
    if (!source) return E_OUTOFMEMORY;
    const HRESULT hr = source->init(initial);
    if (FAILED(hr)) {
        if (source->stream_) source->stream_->shutdown(); // breaks the stream -> source reference
        source->Shutdown();
        source->Release();
        return hr;
    }
    *out = source;
    return S_OK;
}

HRESULT MediaSource::init(IMFAttributes* initial) {
    HRESULT hr = MFCreateAttributes(&attributes_, 4);
    if (FAILED(hr)) return hr;
    if (initial && FAILED(hr = initial->CopyAllItems(attributes_.Get()))) return hr;

    // Camera profile: only the mandatory legacy profile, for profile-aware apps
    ComPtr<IMFSensorProfileCollection> profiles;
    ComPtr<IMFSensorProfile> legacy;
    if (SUCCEEDED(MFCreateSensorProfileCollection(&profiles)) &&
        SUCCEEDED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &legacy)) &&
        SUCCEEDED(legacy->AddProfileFilter(kStreamId, L"((RES==;FRT<=30,1;SUT==))")) &&
        SUCCEEDED(profiles->AddProfile(legacy.Get()))) {
        attributes_->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.Get());
    }

    if (FAILED(hr = MFCreateEventQueue(&queue_))) return hr;
    if (FAILED(hr = MediaStream::create(static_cast<IMFMediaSourceEx*>(this), kStreamId, &stream_))) return hr;

    IMFStreamDescriptor* descriptors[] = { stream_->descriptor() };
    if (FAILED(hr = MFCreatePresentationDescriptor(1, descriptors, &descriptor_))) return hr;
    return descriptor_->SelectStream(0);
}

bool MediaSource::isShutdown() {
    std::lock_guard lock(mutex_);
    return !queue_;
}

STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) ||
        riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaSourceEx)) {
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    } else if (riid == __uuidof(IMFGetService)) {
        *ppv = static_cast<IMFGetService*>(this);
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

// ── IMFMediaSource ──────────────────────────────────────────────────────────

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    std::lock_guard lock(mutex_);
    *descriptor = nullptr;
    if (!queue_) return MF_E_SHUTDOWN;
    return descriptor_->Clone(descriptor);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::Pause() {
    std::lock_guard lock(mutex_);
    // Frame Server sources do not support pausing
    return queue_ ? MF_E_INVALID_STATE_TRANSITION : MF_E_SHUTDOWN;
}

STDMETHODIMP MediaSource::Shutdown() {
    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;
    if (stream_) {
        stream_->shutdown();
        stream_.Reset();
    }
    queue_->Shutdown();
    queue_.Reset();
    descriptor_.Reset();
    trace(L"BlackCameraVCam: source shut down");
    return S_OK;
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition) {
    if (!descriptor || !startPosition) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;

    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;

    DWORD count = 0;
    HRESULT hr = descriptor->GetStreamDescriptorCount(&count);
    if (FAILED(hr)) return hr;
    if (count != 1) return E_INVALIDARG;

    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> streamDescriptor;
    if (FAILED(hr = descriptor->GetStreamDescriptorByIndex(0, &selected, &streamDescriptor))) return hr;
    DWORD id = 0;
    if (FAILED(hr = streamDescriptor->GetStreamIdentifier(&id))) return hr;
    if (id != kStreamId) return MF_E_INVALIDSTREAMNUMBER;

    if (selected) {
        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> type;
        if (SUCCEEDED(streamDescriptor->GetMediaTypeHandler(&handler))) handler->GetCurrentMediaType(&type);

        descriptor_->SelectStream(0);
        // New stream on the first start, updated stream on later ones
        const MediaEventType event = streamSelectedBefore_ ? MEUpdatedStream : MENewStream;
        if (FAILED(hr = queue_->QueueEventParamUnk(event, GUID_NULL, S_OK, static_cast<IMFMediaStream2*>(stream_.Get())))) return hr;
        if (FAILED(hr = stream_->start(type.Get()))) return hr;
    } else {
        descriptor_->DeselectStream(0);
        MF_STREAM_STATE state;
        if (SUCCEEDED(stream_->GetStreamState(&state)) && state != MF_STREAM_STATE_STOPPED) stream_->stop();
    }
    streamSelectedBefore_ = selected != FALSE;

    PROPVARIANT now;
    InitPropVariantFromInt64(MFGetSystemTime(), &now);
    return queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &now);
}

STDMETHODIMP MediaSource::Stop() {
    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;
    MF_STREAM_STATE state;
    if (SUCCEEDED(stream_->GetStreamState(&state)) && state != MF_STREAM_STATE_STOPPED) stream_->stop();
    PROPVARIANT now;
    InitPropVariantFromInt64(MFGetSystemTime(), &now);
    return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &now);
}

// ── IMFMediaSourceEx ────────────────────────────────────────────────────────

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    std::lock_guard lock(mutex_);
    *attributes = nullptr;
    if (!queue_) return MF_E_SHUTDOWN;
    return attributes_.CopyTo(attributes);
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    std::lock_guard lock(mutex_);
    *attributes = nullptr;
    if (!queue_) return MF_E_SHUTDOWN;
    if (streamId != kStreamId) return MF_E_INVALIDSTREAMNUMBER;
    return stream_->QueryInterface(IID_PPV_ARGS(attributes));
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*) {
    // Frames are produced in system memory (MFCreate2DMediaBuffer); the
    // pipeline uploads them to the GPU itself when it needs to.
    std::lock_guard lock(mutex_);
    return queue_ ? S_OK : MF_E_SHUTDOWN;
}

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// ── IMFMediaEventGenerator ──────────────────────────────────────────────────

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(result, event);
}

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    // May block: do not hold the lock while waiting
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard lock(mutex_);
        if (!queue_) return MF_E_SHUTDOWN;
        queue = queue_;
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID extType, HRESULT status, const PROPVARIANT* value) {
    std::lock_guard lock(mutex_);
    if (!queue_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(type, extType, status, value);
}

} // namespace vcamdll
