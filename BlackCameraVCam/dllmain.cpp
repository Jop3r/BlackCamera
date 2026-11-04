// BlackCameraVCam.dll: in-process COM server for the BlackCamera virtual camera.
//
//   regsvr32 BlackCameraVCam.dll      (elevated; the installer does this)
//
// registers the media source under HKLM, the only place the Frame Server
// service (LOCAL SERVICE) looks. BlackCamera.exe then creates the camera with
// MFCreateVirtualCamera(), pointing at vcam::kSourceClsidString.

#include "activator.h"
#include "common.h"
#include "vcam_shared.h"

#include <objbase.h>

#include <cstdarg>
#include <string>

namespace vcamdll {

std::atomic<long> g_moduleRefs{ 0 };

void trace(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
}

} // namespace vcamdll

using namespace vcamdll;

namespace {

HMODULE g_module = nullptr;

const CLSID& sourceClsid() {
    static const CLSID clsid = [] {
        CLSID c{};
        CLSIDFromString(vcam::kSourceClsidString, &c);
        return c;
    }();
    return clsid;
}

class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    // Static object: the reference count only keeps the module loaded
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++g_moduleRefs; }
    STDMETHODIMP_(ULONG) Release() override { return (ULONG)--g_moduleRefs; }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        Activator* activator = nullptr;
        HRESULT hr = Activator::create(&activator);
        if (FAILED(hr)) return hr;
        hr = activator->QueryInterface(riid, ppv);
        activator->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        lock ? ++g_moduleRefs : --g_moduleRefs;
        return S_OK;
    }
};

ClassFactory g_factory;

std::wstring clsidKeyPath() {
    return std::wstring(L"Software\\Classes\\CLSID\\") + vcam::kSourceClsidString;
}

LSTATUS setString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() {
    return g_moduleRefs.load() == 0 ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != sourceClsid()) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

// Frame Server only reads HKLM, so per-user registration is not an option
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return HRESULT_FROM_WIN32(GetLastError());

    HKEY key = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, clsidKeyPath().c_str(), 0, nullptr, 0,
                                 KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    st = setString(key, nullptr, L"BlackCamera Virtual Camera Source");
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);

    st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, (clsidKeyPath() + L"\\InprocServer32").c_str(), 0, nullptr, 0,
                         KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    st = setString(key, nullptr, path);
    if (st == ERROR_SUCCESS) st = setString(key, L"ThreadingModel", L"Both");
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(st);
}

STDAPI DllUnregisterServer() {
    HKEY classes = nullptr;
    LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID", 0,
                               DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &classes);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    st = RegDeleteTreeW(classes, vcam::kSourceClsidString);
    RegCloseKey(classes);
    return (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(st);
}
