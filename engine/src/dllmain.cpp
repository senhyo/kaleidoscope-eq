// In-proc COM server: class factory and self-registration (regsvr32).
#include <windows.h>
#include <olectl.h>

#include <cstdlib>
#include <cwchar>
#include <new>

#include "apo.h"
#include "files.h"
#include "guids.h"

// The DLL is built without C++ exceptions, so its allocator does not throw either: out of memory is nullptr, which
// every `new` here (and in audiomediatypecrt.lib) checks. The CRT's own operator new would throw std::bad_alloc
// and pull the whole C++ exception runtime into the DLL for that one case.
void* operator new(size_t n) { return std::malloc(n ? n : 1); }
void* operator new[](size_t n) { return std::malloc(n ? n : 1); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return std::malloc(n ? n : 1); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return std::malloc(n ? n : 1); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {

HMODULE g_module = nullptr;
std::atomic<long> g_locks{0};

class Factory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }  // static object
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        // audiodg aggregates APOs; COM only allows that when the outer object asks for IUnknown.
        if (outer && riid != __uuidof(IUnknown)) return CLASS_E_NOAGGREGATION;
        KeqApo* apo = new (std::nothrow) KeqApo(outer);
        if (!apo) return E_OUTOFMEMORY;
        IUnknown* inner = apo->Inner();
        const HRESULT hr = inner->QueryInterface(riid, ppv);
        inner->Release();
        if (FAILED(hr)) keq::Trace("CreateInstance riid=%08lx-%04x-%04x failed hr=%08lx", riid.Data1, riid.Data2, riid.Data3, (unsigned long)hr);
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        lock ? ++g_locks : --g_locks;
        return S_OK;
    }
};
Factory g_factory;

LSTATUS SetString(HKEY root, const wchar_t* key, const wchar_t* name, const wchar_t* value) {
    return RegSetKeyValueW(root, key, name, REG_SZ, value, (DWORD)((std::wcslen(value) + 1) * sizeof(wchar_t)));
}
LSTATUS SetDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD value) {
    return RegSetKeyValueW(root, key, name, REG_DWORD, &value, sizeof value);
}

#define KEQ_CLSID_KEY L"SOFTWARE\\Classes\\CLSID\\" KEQ_CLSID_STRING
const wchar_t kClsidKey[] = KEQ_CLSID_KEY;
const wchar_t kInprocKey[] = KEQ_CLSID_KEY L"\\InprocServer32";
const wchar_t kApoKey[] = L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\" KEQ_CLSID_STRING;

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != CLSID_KaleidoscopeEqApo) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

// COM class + the audio engine's APO registry entry, both from KeqRegProperties() (the same struct the APO reports).
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return SELFREG_E_CLASS;

    const APO_REG_PROPERTIES& p = KeqRegProperties();
    LSTATUS e = ERROR_SUCCESS;
    auto ok = [&](LSTATUS r) { if (r != ERROR_SUCCESS && e == ERROR_SUCCESS) e = r; };
    ok(SetString(HKEY_LOCAL_MACHINE, kClsidKey, nullptr, p.szFriendlyName));
    ok(SetString(HKEY_LOCAL_MACHINE, kInprocKey, nullptr, path));
    ok(SetString(HKEY_LOCAL_MACHINE, kInprocKey, L"ThreadingModel", L"Both"));
    ok(SetString(HKEY_LOCAL_MACHINE, kApoKey, L"FriendlyName", p.szFriendlyName));
    ok(SetString(HKEY_LOCAL_MACHINE, kApoKey, L"Copyright", p.szCopyrightInfo));
    ok(SetDword(HKEY_LOCAL_MACHINE, kApoKey, L"MajorVersion", p.u32MajorVersion));
    ok(SetDword(HKEY_LOCAL_MACHINE, kApoKey, L"MinorVersion", p.u32MinorVersion));
    ok(SetDword(HKEY_LOCAL_MACHINE, kApoKey, L"Flags", (DWORD)p.Flags));
    ok(SetDword(HKEY_LOCAL_MACHINE, kApoKey, L"MinInputConnections", p.u32MinInputConnections));
    ok(SetDword(HKEY_LOCAL_MACHINE, kApoKey, L"MaxInputConnections", p.u32MaxInputConnections));
    ok(SetDword(HKEY_LOCAL_MACHINE, kApoKey, L"MinOutputConnections", p.u32MinOutputConnections));
    ok(SetDword(HKEY_LOCAL_MACHINE, kApoKey, L"MaxOutputConnections", p.u32MaxOutputConnections));
    ok(SetDword(HKEY_LOCAL_MACHINE, kApoKey, L"MaxInstances", p.u32MaxInstances));
    ok(SetDword(HKEY_LOCAL_MACHINE, kApoKey, L"NumAPOInterfaces", p.u32NumAPOInterfaces));
    for (UINT32 i = 0; i < p.u32NumAPOInterfaces; ++i) {
        wchar_t name[32], iid[40];
        _snwprintf_s(name, _TRUNCATE, L"APOInterface%u", i);
        StringFromGUID2(p.iidAPOInterfaceList[i], iid, 40);
        ok(SetString(HKEY_LOCAL_MACHINE, kApoKey, name, iid));
    }
    return e == ERROR_SUCCESS ? S_OK : SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer() {
    const LSTATUS a = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsidKey);
    const LSTATUS b = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kApoKey);
    auto gone = [](LSTATUS r) { return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND; };
    return gone(a) && gone(b) ? S_OK : SELFREG_E_CLASS;
}
