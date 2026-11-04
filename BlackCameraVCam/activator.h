#pragma once
// The COM class registered under vcam::kSourceClsidString. Frame Server creates
// it, fills its attribute store (symbolic link name, client context, ...) and
// calls ActivateObject() to get the media source.

#include "common.h"
#include "media_source.h"

#include <mutex>

namespace vcamdll {

class Activator final : public ComObject, public IMFActivate {
public:
    static HRESULT create(Activator** out);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    VCAM_IUNKNOWN_REFCOUNT

    // IMFAttributes (base of IMFActivate)
    VCAM_IMFATTRIBUTES_DELEGATE(attributes_.Get())

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

private:
    Activator() = default;
    ~Activator() override = default;

    std::mutex mutex_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<MediaSource> source_;
};

} // namespace vcamdll
