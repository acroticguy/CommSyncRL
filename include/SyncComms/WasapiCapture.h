#pragma once

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <functional>
#include <thread>
#include <cstdint>

namespace SyncComms {

/// WASAPI-based audio capture supporting per-process loopback (Windows 10 2004+)
/// and full system loopback as fallback.
class WasapiCapture : public IActivateAudioInterfaceCompletionHandler {
public:
    using DataCallback = std::function<void(const float*, uint32_t frameCount, int channels)>;

    WasapiCapture();
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    /// Start capture. If targetPid > 0, attempts per-process loopback.
    /// Falls back to full system loopback on failure.
    /// The callback is invoked on the capture thread with float32 interleaved samples.
    bool Start(uint32_t targetPid, int requestedSampleRate, int requestedChannels,
               DataCallback callback);

    void Stop();
    bool IsCapturing() const;

    /// The actual format negotiated with WASAPI.
    int GetActualSampleRate() const { return m_actualSampleRate; }
    int GetActualChannels() const { return m_actualChannels; }

    /// True if per-process capture is active (vs full loopback fallback).
    bool IsPerProcessActive() const { return m_perProcessActive; }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IActivateAudioInterfaceCompletionHandler
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override;

private:
    bool StartPerProcessLoopback(uint32_t targetPid);
    bool StartFullLoopback();
    bool SetupCaptureFromClient();
    void CaptureThreadProc();
    void Cleanup();

    std::atomic<ULONG> m_refCount{1};

    IAudioClient*        m_audioClient   = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;

    HANDLE m_captureEvent            = nullptr;
    HANDLE m_activationCompleteEvent = nullptr;
    HRESULT m_activationResult       = E_FAIL;

    std::thread       m_captureThread;
    std::atomic<bool> m_running{false};
    DataCallback      m_callback;

    WAVEFORMATEX* m_captureFormat = nullptr;
    int m_actualSampleRate = 0;
    int m_actualChannels   = 0;
    bool m_perProcessActive = false;
};

} // namespace SyncComms
