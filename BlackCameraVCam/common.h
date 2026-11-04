#pragma once
// Shared includes and helpers for BlackCameraVCam.dll, the Media Foundation
// media source behind the "BlackCamera" Windows 11 virtual camera.
//
// The DLL is loaded by the Windows Camera Frame Server service (LOCAL SERVICE,
// session 0) and by camera client processes. It must stay light: no OpenCV,
// no vcpkg dependencies, static CRT, nothing written to disk.

#include <windows.h>
#include <unknwn.h>
#include <winternl.h> // NTSTATUS for ks.h
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>  // user-mode IKsControl
#include <wrl/client.h>

#include <atomic>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace vcamdll {

// Live COM objects + LockServer count; DllCanUnloadNow checks it
extern std::atomic<long> g_moduleRefs;

// Debug output, visible with DebugView / a debugger attached to the Frame Server
void trace(const wchar_t* fmt, ...);

// Base for every COM object in the DLL: reference count + module lifetime
class ComObject {
public:
    ComObject() { ++g_moduleRefs; }
    virtual ~ComObject() { --g_moduleRefs; }
    ComObject(const ComObject&) = delete;
    ComObject& operator=(const ComObject&) = delete;

protected:
    ULONG addRef() { return ++refs_; }
    ULONG release() {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

private:
    std::atomic<ULONG> refs_{ 1 };
};

} // namespace vcamdll

#define VCAM_IUNKNOWN_REFCOUNT                                   \
    STDMETHODIMP_(ULONG) AddRef() override { return addRef(); }  \
    STDMETHODIMP_(ULONG) Release() override { return release(); }

// Forwards every IMFAttributes method to the IMFAttributes* expression `store`
#define VCAM_IMFATTRIBUTES_DELEGATE(store)                                                                                       \
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return (store)->GetItem(k, v); }                                  \
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return (store)->GetItemType(k, t); }                    \
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return (store)->CompareItem(k, v, r); }            \
    STDMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return (store)->Compare(a, m, r); }   \
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return (store)->GetUINT32(k, v); }                                   \
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return (store)->GetUINT64(k, v); }                                   \
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return (store)->GetDouble(k, v); }                                   \
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return (store)->GetGUID(k, v); }                                         \
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* n) override { return (store)->GetStringLength(k, n); }                       \
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 c, UINT32* n) override { return (store)->GetString(k, s, c, n); }         \
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* n) override { return (store)->GetAllocatedString(k, s, n); }   \
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return (store)->GetBlobSize(k, n); }                               \
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 c, UINT32* n) override { return (store)->GetBlob(k, b, c, n); }             \
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return (store)->GetAllocatedBlob(k, b, n); }       \
    STDMETHODIMP GetUnknown(REFGUID k, REFIID i, LPVOID* p) override { return (store)->GetUnknown(k, i, p); }                    \
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return (store)->SetItem(k, v); }                                \
    STDMETHODIMP DeleteItem(REFGUID k) override { return (store)->DeleteItem(k); }                                               \
    STDMETHODIMP DeleteAllItems() override { return (store)->DeleteAllItems(); }                                                 \
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return (store)->SetUINT32(k, v); }                                    \
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return (store)->SetUINT64(k, v); }                                    \
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return (store)->SetDouble(k, v); }                                    \
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return (store)->SetGUID(k, v); }                                       \
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return (store)->SetString(k, v); }                                   \
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return (store)->SetBlob(k, b, n); }                     \
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return (store)->SetUnknown(k, u); }                               \
    STDMETHODIMP LockStore() override { return (store)->LockStore(); }                                                           \
    STDMETHODIMP UnlockStore() override { return (store)->UnlockStore(); }                                                       \
    STDMETHODIMP GetCount(UINT32* n) override { return (store)->GetCount(n); }                                                   \
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return (store)->GetItemByIndex(i, k, v); }         \
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return (store)->CopyAllItems(d); }

// IKsControl with no supported controls, the documented way for a Frame Server source
#define VCAM_IKSCONTROL_NONE                                                                       \
    STDMETHODIMP_(NTSTATUS) KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* n) override {     \
        if (n) *n = 0;                                                                             \
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);                                            \
    }                                                                                              \
    STDMETHODIMP_(NTSTATUS) KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* n) override {         \
        if (n) *n = 0;                                                                             \
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);                                            \
    }                                                                                              \
    STDMETHODIMP_(NTSTATUS) KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* n) override {           \
        if (n) *n = 0;                                                                             \
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);                                            \
    }
