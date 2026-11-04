#pragma once
// The single video stream of the virtual camera: 1920x1080 @ 30 FPS, NV12 or YUY2.
//
// Frame Server pulls samples with RequestSample(); requests are queued and a
// worker thread answers them at the camera frame rate, the way a real camera
// delivers frames as they come off the sensor.

#include "common.h"
#include "frame_reader.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace vcamdll {

class MediaStream final : public ComObject, public IMFMediaStream2, public IKsControl, public IMFAttributes {
public:
    static HRESULT create(IMFMediaSource* source, DWORD streamId, MediaStream** out);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    VCAM_IUNKNOWN_REFCOUNT

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extType, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // IKsControl: no camera controls
    VCAM_IKSCONTROL_NONE

    // IMFAttributes: the stream attribute store (category, id, sharing, ...)
    VCAM_IMFATTRIBUTES_DELEGATE(attributes_.Get())

    // Called by the media source (under its lock)
    HRESULT start(IMFMediaType* type); // type may be null to keep the current format
    HRESULT stop();
    void shutdown();
    IMFStreamDescriptor* descriptor() const { return descriptor_.Get(); }

private:
    MediaStream() = default;
    ~MediaStream() override { shutdown(); }

    HRESULT init(IMFMediaSource* source, DWORD streamId);
    void deliveryLoop();
    HRESULT buildSample(const GUID& subtype, IMFSample** out);

    std::mutex mutex_;                    // guards everything below except the reader
    std::condition_variable wake_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFStreamDescriptor> descriptor_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFMediaSource> source_;       // cycle broken in shutdown()
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    GUID subtype_ = GUID_NULL;
    std::deque<ComPtr<IUnknown>> pending_; // tokens of outstanding RequestSample calls
    bool quit_ = false;
    std::thread worker_;

    // Used only by the worker thread
    SharedFrameReader reader_;
    std::vector<uint8_t> frame_;
};

} // namespace vcamdll
