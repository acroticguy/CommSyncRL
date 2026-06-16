#include "SyncComms/WasapiCapture.h"

#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mfapi.h>   // MFStartup / MFPutWorkItem2 / MFASYNC_CALLBACK_QUEUE_MULTITHREADED

#include <iostream>
#include <vector>
#include <cstring>

#pragma comment(lib, "mfplat.lib")

// Per-process loopback structures (Windows 10 2004+ / SDK 10.0.19041+)
#ifndef AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
#define AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK 1

typedef enum {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

typedef struct {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

typedef struct {
    int ActivationType; // AUDIOCLIENT_ACTIVATION_TYPE
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;
#endif

// Virtual audio device ID for process loopback
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
static const WCHAR VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK[] =
    L"VAD\\Process_Loopback";
#endif

namespace SyncComms {

WasapiCapture::WasapiCapture() {
    m_activationCompleteEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // Needed so MFPutWorkItem2 has a running work-queue system. Ref-counted by
    // MF, so it's safe even if the host already called MFStartup.
    m_mfStarted = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}

WasapiCapture::~WasapiCapture() {
    Stop();
    if (m_activationCompleteEvent) {
        CloseHandle(m_activationCompleteEvent);
        m_activationCompleteEvent = nullptr;
    }
    if (m_mfStarted) {
        MFShutdown();
        m_mfStarted = false;
    }
}

// IUnknown
HRESULT STDMETHODCALLTYPE WasapiCapture::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
        // Disambiguate the IUnknown diamond via the activation-handler base.
        *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
    } else if (riid == __uuidof(IMFAsyncCallback)) {
        *ppv = static_cast<IMFAsyncCallback*>(this);
    } else if (riid == __uuidof(IAgileObject)) {
        // ActivateAudioInterfaceAsync requires the completion handler to be
        // agile (the MS WinRT sample's handler is, ours must say so too or the
        // call returns E_ILLEGAL_METHOD_CALL). IAgileObject is a marker with no
        // methods, so any valid IUnknown for this object satisfies it.
        *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

// IMFAsyncCallback
HRESULT STDMETHODCALLTYPE WasapiCapture::GetParameters(DWORD*, DWORD*) {
    return E_NOTIMPL;  // use default queue/flags
}

// Runs on an MF multithreaded work-queue thread — the required context for
// ActivateAudioInterfaceAsync with the process-loopback virtual device.
HRESULT STDMETHODCALLTYPE WasapiCapture::Invoke(IMFAsyncResult*) {
    AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
    activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activationParams.ProcessLoopbackParams.TargetProcessId = m_pendingActivatePid;
    activationParams.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams = {};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(activationParams);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParams);

    IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activateParams,
        static_cast<IActivateAudioInterfaceCompletionHandler*>(this),
        &asyncOp);
    if (asyncOp) asyncOp->Release();

    // On synchronous failure, ActivateCompleted won't fire — signal here so the
    // waiting Start() thread doesn't time out.
    if (FAILED(hr)) {
        std::cerr << "[WasapiCapture] ActivateAudioInterfaceAsync failed hr=0x"
                  << std::hex << hr << std::dec << "\n";
        m_activationResult = hr;
        SetEvent(m_activationCompleteEvent);
    }
    return S_OK;
}

ULONG STDMETHODCALLTYPE WasapiCapture::AddRef() {
    return m_refCount.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE WasapiCapture::Release() {
    ULONG count = m_refCount.fetch_sub(1) - 1;
    // prevent self-deletion: this object is not COM-allocated
    return count;
}

// IActivateAudioInterfaceCompletionHandler
HRESULT STDMETHODCALLTYPE WasapiCapture::ActivateCompleted(
    IActivateAudioInterfaceAsyncOperation* op)
{
    HRESULT hrActivate = E_FAIL;
    IUnknown* punkResult = nullptr;

    HRESULT hr = op->GetActivateResult(&hrActivate, &punkResult);
    if (SUCCEEDED(hr) && SUCCEEDED(hrActivate) && punkResult) {
        punkResult->QueryInterface(__uuidof(IAudioClient),
                                    reinterpret_cast<void**>(&m_audioClient));
        punkResult->Release();
        m_activationResult = S_OK;
    } else {
        m_activationResult = FAILED(hrActivate) ? hrActivate : hr;
    }

    SetEvent(m_activationCompleteEvent);
    return S_OK;
}

bool WasapiCapture::Start(uint32_t targetPid, int requestedSampleRate, int requestedChannels,
                           DataCallback callback)
{
    if (m_running) return false;
    m_callback = std::move(callback);

    const int sampleRate = requestedSampleRate > 0 ? requestedSampleRate : 48000;
    const int channels   = requestedChannels   > 0 ? requestedChannels   : 2;

    // Try per-process loopback first (isolates the selected app's audio).
    if (targetPid > 0) {
        if (StartPerProcessLoopback(targetPid, sampleRate, channels)) {
            m_perProcessActive = true;
            std::cerr << "[WasapiCapture] per-process loopback ACTIVE for pid "
                      << targetPid << " (" << m_actualSampleRate << "Hz/"
                      << m_actualChannels << "ch)\n";
            return true;
        }
        // Loud: a target WAS requested but isolation failed. Falling back to
        // full-system loopback means we record the ENTIRE mix (game + comms +
        // everything), which is almost never what the user wanted.
        std::cerr << "[WasapiCapture] WARNING: per-process loopback FAILED for pid "
                  << targetPid << " — falling back to FULL-SYSTEM capture "
                     "(you will hear game audio + everything, not just the "
                     "selected app)\n";
    } else {
        std::cerr << "[WasapiCapture] no target pid — using full-system loopback\n";
    }

    // Fallback to full system loopback
    m_perProcessActive = false;
    bool ok = StartFullLoopback();
    std::cerr << "[WasapiCapture] full-system loopback "
              << (ok ? "active" : "FAILED") << "\n";
    return ok;
}

bool WasapiCapture::StartPerProcessLoopback(uint32_t targetPid, int sampleRate, int channels) {
    // Reset activation state and stash the pid for Invoke().
    m_activationResult = E_FAIL;
    m_audioClient = nullptr;
    m_pendingActivatePid = targetPid;
    ResetEvent(m_activationCompleteEvent);

    // ActivateAudioInterfaceAsync for process loopback must be issued from an
    // MF multithreaded work-queue thread (otherwise E_ILLEGAL_METHOD_CALL).
    // Dispatch Invoke() there; it calls ActivateAudioInterfaceAsync and the
    // result arrives in ActivateCompleted, which signals m_activationCompleteEvent.
    HRESULT hr = MFPutWorkItem2(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 0,
                                static_cast<IMFAsyncCallback*>(this), nullptr);
    if (FAILED(hr)) {
        std::cerr << "[WasapiCapture] MFPutWorkItem2 failed hr=0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    // Wait for activation to complete (5 second timeout)
    DWORD waitResult = WaitForSingleObject(m_activationCompleteEvent, 5000);

    if (waitResult != WAIT_OBJECT_0 || FAILED(m_activationResult) || !m_audioClient) {
        std::cerr << "[WasapiCapture] process-loopback activation failed (wait="
                  << waitResult << " result=0x" << std::hex << m_activationResult
                  << std::dec << ")\n";
        Cleanup();
        return false;
    }

    // CRITICAL: the process-loopback virtual device does NOT support
    // GetMixFormat() — calling it (as the full-loopback path does) fails and
    // is what silently dropped us to full-system capture. Supply a fixed
    // float32 format instead, matching Microsoft's ApplicationLoopback sample.
    // The virtual device converts the app's audio to this format for us.
    //
    // Force STEREO regardless of the config's channel count: stereo float32 is
    // the most reliably-accepted loopback format, and the pipeline keys off
    // GetActualChannels() anyway (the old full-loopback path captured at the
    // endpoint's mix format, which is stereo) — so this changes nothing
    // downstream while avoiding a mono-format rejection that would silently
    // fall back to full-system capture.
    (void)channels;
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels       = 2;
    fmt.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
    fmt.wBitsPerSample  = 32;
    fmt.nBlockAlign     = static_cast<WORD>((fmt.nChannels * fmt.wBitsPerSample) / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize          = 0;

    return SetupCaptureFromClient(&fmt, /*takeOwnership=*/false);
}

bool WasapiCapture::StartFullLoopback() {
    // Classic WASAPI loopback via IMMDevice
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInit = SUCCEEDED(hrCom);

    IMMDeviceEnumerator* deviceEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&deviceEnum));
    if (FAILED(hr) || !deviceEnum) {
        if (comInit) CoUninitialize();
        return false;
    }

    IMMDevice* device = nullptr;
    hr = deviceEnum->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    deviceEnum->Release();
    if (FAILED(hr) || !device) {
        if (comInit) CoUninitialize();
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&m_audioClient));
    device->Release();
    if (comInit) CoUninitialize();

    if (FAILED(hr) || !m_audioClient) return false;

    // Real render endpoint DOES support GetMixFormat (usually float32). The
    // returned format is CoTaskMemAlloc'd, so SetupCaptureFromClient takes
    // ownership and frees it in Cleanup.
    WAVEFORMATEX* mixFormat = nullptr;
    hr = m_audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        Cleanup();
        return false;
    }
    return SetupCaptureFromClient(mixFormat, /*takeOwnership=*/true);
}

bool WasapiCapture::SetupCaptureFromClient(WAVEFORMATEX* format, bool takeOwnership) {
    if (!m_audioClient || !format) return false;

    if (takeOwnership) m_captureFormat = format;  // CoTaskMemFree'd in Cleanup

    m_actualSampleRate = static_cast<int>(format->nSamplesPerSec);
    m_actualChannels   = static_cast<int>(format->nChannels);

    // Initialize shared-mode loopback with event-driven buffering. Same flags
    // for both paths; only the format source differs (fixed for process
    // loopback, GetMixFormat for full loopback).
    HRESULT hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0, 0, format, nullptr);

    if (FAILED(hr)) {
        std::cerr << "[WasapiCapture] IAudioClient::Initialize failed hr=0x"
                  << std::hex << hr << std::dec << "\n";
        Cleanup();
        return false;
    }

    // Set up event-driven capture
    m_captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = m_audioClient->SetEventHandle(m_captureEvent);
    if (FAILED(hr)) {
        Cleanup();
        return false;
    }

    hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient),
                                    reinterpret_cast<void**>(&m_captureClient));
    if (FAILED(hr) || !m_captureClient) {
        Cleanup();
        return false;
    }

    // Start capture
    hr = m_audioClient->Start();
    if (FAILED(hr)) {
        Cleanup();
        return false;
    }

    m_running = true;
    m_captureThread = std::thread(&WasapiCapture::CaptureThreadProc, this);
    return true;
}

void WasapiCapture::CaptureThreadProc() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Boost thread priority for audio
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

    std::vector<float> silenceBuffer;

    while (m_running) {
        DWORD waitResult = WaitForSingleObject(m_captureEvent, 200);
        if (!m_running) break;
        if (waitResult != WAIT_OBJECT_0) continue;

        // Drain all available packets
        while (true) {
            BYTE* pData = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            HRESULT hr = m_captureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY || FAILED(hr) || numFrames == 0) {
                if (SUCCEEDED(hr) && numFrames == 0) {
                    m_captureClient->ReleaseBuffer(numFrames);
                }
                break;
            }

            if (m_callback) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    size_t needed = static_cast<size_t>(numFrames) * m_actualChannels;
                    if (silenceBuffer.size() < needed) {
                        silenceBuffer.resize(needed, 0.0f);
                    } else {
                        std::memset(silenceBuffer.data(), 0, needed * sizeof(float));
                    }
                    m_callback(silenceBuffer.data(), numFrames, m_actualChannels);
                } else {
                    m_callback(reinterpret_cast<const float*>(pData), numFrames, m_actualChannels);
                }
            }

            m_captureClient->ReleaseBuffer(numFrames);
        }
    }

    if (hTask) AvRevertMmThreadCharacteristics(hTask);
    CoUninitialize();
}

void WasapiCapture::Stop() {
    if (!m_running) return;

    m_running = false;
    if (m_captureEvent) SetEvent(m_captureEvent); // wake the thread
    if (m_captureThread.joinable()) m_captureThread.join();

    if (m_audioClient) m_audioClient->Stop();

    Cleanup();
}

bool WasapiCapture::IsCapturing() const {
    return m_running;
}

void WasapiCapture::Cleanup() {
    if (m_captureClient) { m_captureClient->Release(); m_captureClient = nullptr; }
    if (m_audioClient) { m_audioClient->Release(); m_audioClient = nullptr; }
    if (m_captureFormat) { CoTaskMemFree(m_captureFormat); m_captureFormat = nullptr; }
    if (m_captureEvent) { CloseHandle(m_captureEvent); m_captureEvent = nullptr; }
    m_actualSampleRate = 0;
    m_actualChannels = 0;
}

} // namespace SyncComms
