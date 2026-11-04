#include "activator.h"

namespace vcamdll {

HRESULT Activator::create(Activator** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    auto* activator = new (std::nothrow) Activator();
    if (!activator) return E_OUTOFMEMORY;
    const HRESULT hr = MFCreateAttributes(&activator->attributes_, 4);
    if (FAILED(hr)) {
        activator->Release();
        return hr;
    }
    *out = activator;
    return S_OK;
}

STDMETHODIMP Activator::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate)) {
        *ppv = static_cast<IMFActivate*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    std::lock_guard lock(mutex_);
    if (!source_ || source_->isShutdown()) {
        // The pipeline has filled our attribute store by now; the source gets a copy
        source_.Reset();
        const HRESULT hr = MediaSource::create(attributes_.Get(), &source_);
        if (FAILED(hr)) {
            trace(L"BlackCameraVCam: media source creation failed (0x%08X)", hr);
            return hr;
        }
        trace(L"BlackCameraVCam: media source created (process %lu)", GetCurrentProcessId());
    }
    return source_->QueryInterface(riid, ppv);
}

// Frame Server requires E_NOTIMPL from both; it shuts the source down itself
STDMETHODIMP Activator::ShutdownObject() { return E_NOTIMPL; }
STDMETHODIMP Activator::DetachObject() { return E_NOTIMPL; }

} // namespace vcamdll
