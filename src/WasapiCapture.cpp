#include "SyncComms/WasapiCapture.h"

#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

#include <vector>
#include <cstring>

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
}

WasapiCapture::~WasapiCapture() {
    Stop();
    if (m_activationCompleteEvent) {
        CloseHandle(m_activationCompleteEvent);
        m_activationCompleteEvent = nullptr;
    }
}

// IUnknown
HRESULT STDMETHODCALLTYPE WasapiCapture::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
        *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
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

    // Try per-process loopback first
    if (targetPid > 0 && StartPerProcessLoopback(targetPid)) {
        m_perProcessActive = true;
        return true;
    }

    // Fallback to full system loopback
    m_perProcessActive = false;
    return StartFullLoopback();
}

bool WasapiCapture::StartPerProcessLoopback(uint32_t targetPid) {
    // Set up activation params
    AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
    activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activationParams.ProcessLoopbackParams.TargetProcessId = targetPid;
    activationParams.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams = {};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(activationParams);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParams);

    // Reset activation state
    m_activationResult = E_FAIL;
    m_audioClient = nullptr;
    ResetEvent(m_activationCompleteEvent);

    IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activateParams,
        static_cast<IActivateAudioInterfaceCompletionHandler*>(this),
        &asyncOp);

    if (FAILED(hr)) return false;

    // Wait for activation to complete (5 second timeout)
    DWORD waitResult = WaitForSingleObject(m_activationCompleteEvent, 5000);
    if (asyncOp) asyncOp->Release();

    if (waitResult != WAIT_OBJECT_0 || FAILED(m_activationResult) || !m_audioClient) {
        Cleanup();
        return false;
    }

    return SetupCaptureFromClient();
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

    return SetupCaptureFromClient();
}

bool WasapiCapture::SetupCaptureFromClient() {
    if (!m_audioClient) return false;

    // Get the mix format
    HRESULT hr = m_audioClient->GetMixFormat(&m_captureFormat);
    if (FAILED(hr) || !m_captureFormat) {
        Cleanup();
        return false;
    }

    m_actualSampleRate = static_cast<int>(m_captureFormat->nSamplesPerSec);
    m_actualChannels = static_cast<int>(m_captureFormat->nChannels);

    // Initialize shared-mode loopback with event-driven buffering
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0, 0, m_captureFormat, nullptr);

    if (FAILED(hr)) {
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
