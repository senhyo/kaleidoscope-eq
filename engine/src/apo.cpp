#include "apo.h"

#include <audiomediatype.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <cwchar>
#include <cwctype>

#include "guids.h"
#include "version.h"

std::atomic<long> g_objects{0};

namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// PKEY_AudioEndpoint_GUID
const PROPERTYKEY kEndpointGuidKey = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 4};

bool IsFloat32(const WAVEFORMATEX* w) {
    if (!w || w->nChannels == 0 || w->nSamplesPerSec == 0 || w->wBitsPerSample != 32) return false;
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (w->wFormatTag != WAVE_FORMAT_EXTENSIBLE || w->cbSize < 22) return false;
    const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
    return x->SubFormat == kSubtypeFloat && x->Samples.wValidBitsPerSample == 32;
}

// "{0.0.0.00000000}.{6f716e85-...}" or "{6f716e85-...}" -> "{6f716e85-...}" in out[39]; empty if there is none
template <size_t N>
void EndpointGuid(const wchar_t* id, wchar_t (&out)[N]) {
    out[0] = 0;
    const wchar_t* open = id ? std::wcsrchr(id, L'{') : nullptr;
    const wchar_t* close = open ? std::wcschr(open, L'}') : nullptr;
    if (!close || (size_t)(close - open) >= N - 1) return;
    size_t n = 0;
    for (const wchar_t* c = open; c <= close; ++c) out[n++] = (wchar_t)std::towlower(*c);
    out[n] = 0;
}

}  // namespace

const APO_REG_PROPERTIES& KeqRegProperties() {
    static const APO_REG_PROPERTIES props = {
        CLSID_KaleidoscopeEqApo,
        (APO_FLAG)(APO_FLAG_INPLACE | APO_FLAG_DEFAULT),
        L"Kaleidoscope EQ",                    // szFriendlyName
        L"Kaleidoscope",                       // szCopyrightInfo
        KEQ_VERSION_MAJOR, KEQ_VERSION_MINOR,
        1, 1,                                  // input connections min, max
        1, 1,                                  // output connections min, max
        0xffffffff,                            // u32MaxInstances
        1, {__uuidof(IAudioProcessingObject)}, // APO interfaces
    };
    return props;
}

HRESULT KeqApo::InnerQueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown)) {
        *ppv = &inner_;  // identity is the non-delegating IUnknown
        inner_.AddRef();
        return S_OK;
    }
    if (riid == __uuidof(IAudioProcessingObject))
        *ppv = static_cast<IAudioProcessingObject*>(this);
    else if (riid == __uuidof(IAudioProcessingObjectRT))
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    else if (riid == __uuidof(IAudioProcessingObjectConfiguration))
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    else if (riid == __uuidof(IAudioSystemEffects) || riid == __uuidof(IAudioSystemEffects2))
        *ppv = static_cast<IAudioSystemEffects2*>(this);
    else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP KeqApo::Reset() {
    proc_.Clear();  // called only while not processing: same params, empty delay lines
    return S_OK;
}

STDMETHODIMP KeqApo::GetLatency(HNSTIME* time) {
    if (!time) return E_POINTER;
    *time = 0;
    return S_OK;
}

STDMETHODIMP KeqApo::GetRegistrationProperties(APO_REG_PROPERTIES** props) {
    if (!props) return E_POINTER;
    *props = static_cast<APO_REG_PROPERTIES*>(CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES)));
    if (!*props) return E_OUTOFMEMORY;
    **props = KeqRegProperties();
    return S_OK;
}

STDMETHODIMP KeqApo::Initialize(UINT32 cbDataSize, BYTE* pbyData) {
    if (!pbyData || cbDataSize < sizeof(APOInitBaseStruct)) return S_OK;  // nothing tells us the endpoint: bypass
    const auto* base = reinterpret_cast<const APOInitBaseStruct*>(pbyData);
    if (base->cbSize != cbDataSize) return E_INVALIDARG;

    if (cbDataSize >= sizeof(APOInitSystemEffects2)) {
        const auto* fx2 = reinterpret_cast<const APOInitSystemEffects2*>(pbyData);
        discoveryOnly_ = fx2->InitializeForDiscoveryOnly != FALSE;
        UINT n = 0;
        IMMDevice* dev = nullptr;
        // The last device in the collection is the endpoint this effect runs on.
        if (fx2->pDeviceCollection && SUCCEEDED(fx2->pDeviceCollection->GetCount(&n)) && n > 0 &&
            SUCCEEDED(fx2->pDeviceCollection->Item(n - 1, &dev))) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id))) EndpointGuid(id, endpoint_);
            CoTaskMemFree(id);
            dev->Release();
        }
    }
    if (!endpoint_[0] && cbDataSize >= sizeof(APOInitSystemEffects)) {
        const auto* fx = reinterpret_cast<const APOInitSystemEffects*>(pbyData);
        PROPVARIANT v;
        PropVariantInit(&v);
        if (fx->pAPOEndpointProperties && SUCCEEDED(fx->pAPOEndpointProperties->GetValue(kEndpointGuidKey, &v)) &&
            v.vt == VT_LPWSTR)
            EndpointGuid(v.pwszVal, endpoint_);
        PropVariantClear(&v);
    }
    keq::Trace("Initialize size=%u (base %u fx %u fx2 %u) discovery=%d endpoint=%ls", cbDataSize,
               (unsigned)sizeof(APOInitBaseStruct), (unsigned)sizeof(APOInitSystemEffects),
               (unsigned)sizeof(APOInitSystemEffects2), discoveryOnly_ ? 1 : 0, endpoint_[0] ? endpoint_ : L"(none)");
    return S_OK;
}

HRESULT KeqApo::FormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested, IAudioMediaType** supported) {
    if (!requested || !supported) return E_POINTER;
    *supported = nullptr;
    const WAVEFORMATEX* want = requested->GetAudioFormat();
    // In place: both sides share one format. Follow the other side if it is already fixed.
    const WAVEFORMATEX* shape = opposite ? opposite->GetAudioFormat() : want;
    if (!want || !shape || shape->nChannels == 0 || shape->nSamplesPerSec == 0) return APOERR_FORMAT_NOT_SUPPORTED;
    if (IsFloat32(want) && want->nChannels == shape->nChannels && want->nSamplesPerSec == shape->nSamplesPerSec) {
        requested->AddRef();
        *supported = requested;
        return S_OK;
    }
    WAVEFORMATEXTENSIBLE f = {};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = shape->nChannels;
    f.Format.nSamplesPerSec = shape->nSamplesPerSec;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = (WORD)(4 * shape->nChannels);
    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.SubFormat = kSubtypeFloat;
    if (shape->wFormatTag == WAVE_FORMAT_EXTENSIBLE && shape->cbSize >= 22)
        f.dwChannelMask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(shape)->dwChannelMask;
    else
        f.dwChannelMask = shape->nChannels == 1 ? SPEAKER_FRONT_CENTER
                        : shape->nChannels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    if (FAILED(CreateAudioMediaType(&f.Format, sizeof f, supported))) return APOERR_FORMAT_NOT_SUPPORTED;
    return S_FALSE;
}

STDMETHODIMP KeqApo::IsInputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                            IAudioMediaType** supported) {
    return FormatSupported(opposite, requested, supported);
}

STDMETHODIMP KeqApo::IsOutputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                             IAudioMediaType** supported) {
    return FormatSupported(opposite, requested, supported);
}

STDMETHODIMP KeqApo::GetInputChannelCount(UINT32* count) {
    if (!count) return E_POINTER;
    if (!locked_) return APOERR_NOT_INITIALIZED;
    *count = channels_;
    return S_OK;
}

STDMETHODIMP KeqApo::LockForProcess(UINT32 nIn, APO_CONNECTION_DESCRIPTOR** in, UINT32 nOut,
                                    APO_CONNECTION_DESCRIPTOR** out) {
    if (locked_) return APOERR_APO_LOCKED;
    if (nIn != 1 || nOut != 1 || !in || !out || !in[0] || !out[0] || !in[0]->pFormat || !out[0]->pFormat) {
        keq::Trace("LockForProcess endpoint=%ls rejected: connections in=%u out=%u", endpoint_, nIn, nOut);
        return APOERR_NUM_CONNECTIONS_INVALID;
    }
    const WAVEFORMATEX* f = in[0]->pFormat->GetAudioFormat();
    const WAVEFORMATEX* g = out[0]->pFormat->GetAudioFormat();
    if (!IsFloat32(f) || !IsFloat32(g) || f->nChannels != g->nChannels || f->nSamplesPerSec != g->nSamplesPerSec) {
        keq::Trace("LockForProcess endpoint=%ls rejected: in tag=%u bits=%u ch=%u hz=%lu, out tag=%u bits=%u ch=%u hz=%lu",
                   endpoint_, f ? f->wFormatTag : 0, f ? f->wBitsPerSample : 0, f ? f->nChannels : 0,
                   f ? f->nSamplesPerSec : 0, g ? g->wFormatTag : 0, g ? g->wBitsPerSample : 0,
                   g ? g->nChannels : 0, g ? g->nSamplesPerSec : 0);
        return APOERR_FORMAT_NOT_SUPPORTED;
    }

    channels_ = f->nChannels;
    // allocation happens here, never in APOProcess
    if (!proc_.Init(f->nSamplesPerSec, (int)channels_)) return E_OUTOFMEMORY;
    wchar_t dir[keq::kPathMax], cfg[keq::kPathMax], live[keq::kPathMax];
    if (endpoint_[0] && !discoveryOnly_ && keq::EndpointDir(dir) &&
        _snwprintf_s(cfg, _TRUNCATE, L"%s\\%s.cfg", dir, endpoint_) > 0 &&
        _snwprintf_s(live, _TRUNCATE, L"%s\\%s.live", dir, endpoint_) > 0) {
        proc_.Reset(keq::LoadParams(cfg, f->nSamplesPerSec));
        const bool wrote = keq::WriteLiveFile(live);
        const DWORD error = wrote ? 0 : GetLastError();
        watcher_.Start(cfg, f->nSamplesPerSec, &proc_.Inbox());
        keq::Trace("LockForProcess endpoint=%ls ch=%u hz=%lu live=%d err=%lu", endpoint_, channels_,
                   f->nSamplesPerSec, wrote ? 1 : 0, error);
    } else {
        keq::Trace("LockForProcess endpoint=%ls discovery=%d ch=%u hz=%lu: bypass (no endpoint)",
                   endpoint_[0] ? endpoint_ : L"(none)", discoveryOnly_ ? 1 : 0, channels_, f->nSamplesPerSec);
    }
    locked_ = true;
    return S_OK;
}

STDMETHODIMP KeqApo::UnlockForProcess() {
    if (!locked_) return APOERR_ALREADY_UNLOCKED;
    watcher_.Stop();
    locked_ = false;
    return S_OK;
}

STDMETHODIMP_(void) KeqApo::APOProcess(UINT32 nIn, APO_CONNECTION_PROPERTY** in, UINT32 nOut,
                                       APO_CONNECTION_PROPERTY** out) {
    // Realtime: no locks, no allocation, no IO. Everything below is arithmetic on preallocated state.
    if (nIn < 1 || nOut < 1) return;
    APO_CONNECTION_PROPERTY* src = in[0];
    APO_CONNECTION_PROPERTY* dst = out[0];
    const UINT32 frames = src->u32ValidFrameCount;
    auto* inBuf = reinterpret_cast<float*>(src->pBuffer);
    auto* outBuf = reinterpret_cast<float*>(dst->pBuffer);
    if (outBuf != inBuf) memcpy(outBuf, inBuf, sizeof(float) * frames * channels_);
    const bool wrote = proc_.Process(outBuf, frames, src->u32BufferFlags == BUFFER_SILENT);
    dst->u32ValidFrameCount = frames;
    dst->u32BufferFlags = wrote ? BUFFER_VALID : src->u32BufferFlags;
}

STDMETHODIMP KeqApo::GetEffectsList(LPGUID* ids, UINT* count, HANDLE) {
    if (!ids || !count) return E_POINTER;
    *ids = nullptr;  // nothing for the Windows enhancements UI to toggle; the app is the UI
    *count = 0;
    return S_OK;
}
