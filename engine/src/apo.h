// The APO itself: COM plumbing around keq::Processor. Loaded by audiodg as an endpoint effect (EFX).
#pragma once
#include <windows.h>
#include <audioenginebaseapo.h>

#include <atomic>

#include "dsp.h"
#include "files.h"

extern std::atomic<long> g_objects;  // live COM objects, for DllCanUnloadNow

const APO_REG_PROPERTIES& KeqRegProperties();

// audiodg creates APOs aggregated (CreateInstance with an outer IUnknown, asking for IUnknown), so the object
// follows the COM aggregation rules: `inner_` is the non-delegating IUnknown that owns the reference count, and the
// IUnknown of every other interface delegates to the outer object (to `inner_` itself when not aggregated).
class KeqApo final : public IAudioProcessingObject,
                     public IAudioProcessingObjectRT,
                     public IAudioProcessingObjectConfiguration,
                     public IAudioSystemEffects2 {
public:
    explicit KeqApo(IUnknown* outer = nullptr) : inner_(this), outer_(outer ? outer : &inner_) { ++g_objects; }
    ~KeqApo() { watcher_.Stop(); --g_objects; }

    // The non-delegating IUnknown: what the class factory hands out (refcount starts at 1).
    IUnknown* Inner() { return &inner_; }

    // IUnknown (delegating)
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { return outer_->QueryInterface(riid, ppv); }
    STDMETHODIMP_(ULONG) AddRef() override { return outer_->AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return outer_->Release(); }

    // IAudioProcessingObject
    STDMETHODIMP Reset() override;
    STDMETHODIMP GetLatency(HNSTIME* time) override;
    STDMETHODIMP GetRegistrationProperties(APO_REG_PROPERTIES** props) override;
    STDMETHODIMP Initialize(UINT32 cbDataSize, BYTE* pbyData) override;
    STDMETHODIMP IsInputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                        IAudioMediaType** supported) override;
    STDMETHODIMP IsOutputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                         IAudioMediaType** supported) override;
    STDMETHODIMP GetInputChannelCount(UINT32* count) override;

    // IAudioProcessingObjectRT
    STDMETHODIMP_(void) APOProcess(UINT32 nIn, APO_CONNECTION_PROPERTY** in, UINT32 nOut,
                                   APO_CONNECTION_PROPERTY** out) override;
    STDMETHODIMP_(UINT32) CalcInputFrames(UINT32 frames) override { return frames; }
    STDMETHODIMP_(UINT32) CalcOutputFrames(UINT32 frames) override { return frames; }

    // IAudioProcessingObjectConfiguration
    STDMETHODIMP LockForProcess(UINT32 nIn, APO_CONNECTION_DESCRIPTOR** in, UINT32 nOut,
                                APO_CONNECTION_DESCRIPTOR** out) override;
    STDMETHODIMP UnlockForProcess() override;

    // IAudioSystemEffects2
    STDMETHODIMP GetEffectsList(LPGUID* ids, UINT* count, HANDLE event) override;

private:
    HRESULT FormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested, IAudioMediaType** supported);
    HRESULT InnerQueryInterface(REFIID riid, void** ppv);

    class InnerUnknown final : public IUnknown {
    public:
        explicit InnerUnknown(KeqApo* owner) : owner_(owner) {}
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { return owner_->InnerQueryInterface(riid, ppv); }
        STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
        STDMETHODIMP_(ULONG) Release() override {
            const ULONG n = --refs_;
            if (n == 0) delete owner_;
            return n;
        }

    private:
        KeqApo* owner_;
        std::atomic<ULONG> refs_{1};
    };

    InnerUnknown inner_;
    IUnknown* outer_;  // not owned (an aggregate must not hold a reference on its outer object)
    wchar_t endpoint_[39] = {};  // "{guid}" lower case, empty if unknown
    bool discoveryOnly_ = false;
    bool locked_ = false;
    UINT32 channels_ = 0;
    keq::Processor proc_;
    keq::ConfigWatcher watcher_;
};
