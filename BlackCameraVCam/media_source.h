#pragma once
// Media source of the virtual camera, built to the "Frame Server Custom Media
// Source" requirements: IMFMediaSourceEx + IMFGetService + IKsControl, one
// IMFMediaStream2 stream, live, no seeking or pausing.

#include "common.h"
#include "media_stream.h"

#include <mutex>

namespace vcamdll {

class MediaSource final : public ComObject, public IMFMediaSourceEx, public IMFGetService, public IKsControl, public IMFAttributes {
public:
    // initial: attributes the pipeline set on the activator (copied into the source store)
    static HRESULT create(IMFAttributes* initial, MediaSource** out);
    bool isShutdown();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    VCAM_IUNKNOWN_REFCOUNT

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extType, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition) override;
    STDMETHODIMP Stop() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService: nothing extra to offer
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* ppv) override;

    // IKsControl: no camera controls
    VCAM_IKSCONTROL_NONE

    // IMFAttributes: the source attribute store
    VCAM_IMFATTRIBUTES_DELEGATE(attributes_.Get())

private:
    MediaSource() = default;
    ~MediaSource() override = default;
    HRESULT init(IMFAttributes* initial);

    std::mutex mutex_;
    ComPtr<IMFMediaEventQueue> queue_;          // null after Shutdown()
    ComPtr<IMFPresentationDescriptor> descriptor_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<MediaStream> stream_;
    bool streamSelectedBefore_ = false;
};

} // namespace vcamdll
