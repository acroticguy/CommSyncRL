#include "miniaudio.h"
#include "SyncComms/AudioCaptureManager.h"
#include "SyncComms/AudioSessionEnumerator.h"
#include <filesystem>
#include <algorithm>
#include <cmath>

namespace SyncComms {

// miniaudio mic callback — runs on audio thread, writes to ring buffer
static void micCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pOutput;
    auto* mgr = static_cast<AudioCaptureManager*>(pDevice->pUserData);
    (void)mgr; // we access the ring buffer via the device's user data

    // Get the ring buffer from the manager's context
    // We store a pointer to the RingBuffer in pDevice->pUserData indirectly
    // Actually, we need a struct to hold both pointers. Let's use a simple approach:
    // The mic callback writes directly to a RingBuffer pointer stored after the manager pointer.
    // Simpler: just use a static approach via the manager.
    // Since we can't easily access private members from a static callback,
    // we'll use a small wrapper struct.
}

// Wrapper for mic callback that can access the ring buffer
struct MicCallbackContext {
    RingBuffer* ringBuffer;
    int channels;
};

static void micCallbackWrapper(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pOutput;
    auto* ctx = static_cast<MicCallbackContext*>(pDevice->pUserData);
    if (!ctx || !ctx->ringBuffer || !pInput) return;

    const float* input = static_cast<const float*>(pInput);
    ctx->ringBuffer->Write(input, frameCount * ctx->channels);
}

// Static so it stays alive for the lifetime of the mic device
static MicCallbackContext s_micCallbackCtx = {};

AudioCaptureManager::AudioCaptureManager(std::shared_ptr<SyncState> syncState, Config* config)
    : m_syncState(std::move(syncState))
    , m_config(config)
{
}

AudioCaptureManager::~AudioCaptureManager() {
    if (m_capturing) {
        StopCapture(0);
    }
    StopMicCapture();
}

void AudioCaptureManager::StartCapture(const std::string& replayId, int segmentNumber, int startFrame) {
    std::lock_guard<std::mutex> lock(m_captureMutex);

    if (m_capturing) return;

    m_currentReplayId = replayId;
    m_currentSegment = segmentNumber;
    m_startFrame = startFrame;

    // Ensure output directory exists
    std::string outputDir = m_config->GetOutputDir();
    std::filesystem::create_directories(outputDir);

    // Resolve target process PID
    std::string targetProcess = m_config->GetTargetProcessName();
    uint32_t targetPid = 0;
    if (!targetProcess.empty()) {
        auto pid = AudioSessionEnumerator::FindProcessByName(targetProcess);
        if (pid.has_value()) {
            targetPid = pid.value();
        }
    }

    // Create WASAPI capture
    m_wasapiCapture = std::make_unique<WasapiCapture>();

    int sampleRate = m_config->GetSampleRate();
    int channels = m_config->GetChannels();

    bool started = m_wasapiCapture->Start(targetPid, sampleRate, channels,
        [this](const float* data, uint32_t frameCount, int ch) {
            OnWasapiData(data, frameCount, ch);
        });

    if (!started) {
        m_wasapiCapture.reset();
        return;
    }

    // Use the actual WASAPI format for the WAV file
    m_outputSampleRate = m_wasapiCapture->GetActualSampleRate();
    m_outputChannels = m_wasapiCapture->GetActualChannels();

    // Open WAV file with the actual capture format
    std::string filePath = BuildFilePath(replayId, segmentNumber);
    if (!m_wavWriter.Open(filePath, m_outputSampleRate, m_outputChannels, 32)) {
        m_wasapiCapture->Stop();
        m_wasapiCapture.reset();
        return;
    }

    // Start mic capture if enabled
    if (m_config->GetIncludeMic()) {
        StartMicCapture();
    }

    m_capturing = true;
    m_syncState->isCapturingSegment.store(true);
}

SegmentInfo AudioCaptureManager::StopCapture(int endFrame) {
    std::lock_guard<std::mutex> lock(m_captureMutex);

    SegmentInfo seg{};
    seg.index = m_currentSegment;
    seg.startFrame = m_startFrame;
    seg.endFrame = endFrame;
    seg.frameTime = 1.0 / 30.0;
    seg.audioFile = BuildFilePath(m_currentReplayId, m_currentSegment);
    seg.event = "kickoff_to_goal";

    // Extract just the filename for the sidecar
    std::filesystem::path p(seg.audioFile);
    seg.audioFile = p.filename().string();

    // Stop capture
    StopMicCapture();
    if (m_wasapiCapture) {
        m_wasapiCapture->Stop();
        m_wasapiCapture.reset();
    }
    m_wavWriter.Close();

    m_capturing = false;
    m_syncState->isCapturingSegment.store(false);

    return seg;
}

bool AudioCaptureManager::IsCapturing() const {
    return m_capturing;
}

void AudioCaptureManager::OnWasapiData(const float* data, uint32_t frameCount, int channels) {
    if (!m_capturing || !data) return;

    size_t totalSamples = static_cast<size_t>(frameCount) * channels;

    // If mic is active, mix mic data into the output
    if (m_config->GetIncludeMic() && m_micRingBuffer.AvailableRead() > 0) {
        // Ensure mix buffer is large enough
        if (m_mixBuffer.size() < totalSamples) {
            m_mixBuffer.resize(totalSamples);
        }

        // Copy loopback data
        std::memcpy(m_mixBuffer.data(), data, totalSamples * sizeof(float));

        // Determine how many mic samples to read
        size_t micSamplesNeeded = totalSamples;

        // If mic has different channel count, adjust
        if (m_micChannels != channels) {
            // For simplicity: read mono mic and duplicate to match loopback channels
            micSamplesNeeded = frameCount * m_micChannels;
        }

        // Handle sample rate mismatch between mic and WASAPI
        if (m_micSampleRate != m_outputSampleRate && m_micSampleRate > 0) {
            // Read mic samples at mic's native rate
            double ratio = static_cast<double>(m_micSampleRate) / m_outputSampleRate;
            uint32_t micFramesNeeded = static_cast<uint32_t>(std::ceil(frameCount * ratio));
            size_t micReadCount = micFramesNeeded * m_micChannels;

            if (m_micReadBuffer.size() < micReadCount) {
                m_micReadBuffer.resize(micReadCount);
            }
            size_t read = m_micRingBuffer.Read(m_micReadBuffer.data(), micReadCount);
            uint32_t micFramesRead = static_cast<uint32_t>(read / m_micChannels);

            // Resample mic to match output rate
            if (m_micResampleBuffer.size() < static_cast<size_t>(frameCount) * m_micChannels) {
                m_micResampleBuffer.resize(frameCount * m_micChannels);
            }
            float resampleRatio = static_cast<float>(ratio);
            m_micResampler.Resample(m_micReadBuffer.data(), micFramesRead,
                                     m_micResampleBuffer.data(), frameCount,
                                     m_micChannels, resampleRatio);

            // Mix resampled mic into loopback
            for (uint32_t f = 0; f < frameCount; f++) {
                for (int c = 0; c < channels; c++) {
                    int micCh = (m_micChannels == 1) ? 0 : std::min(c, m_micChannels - 1);
                    m_mixBuffer[f * channels + c] += m_micResampleBuffer[f * m_micChannels + micCh];
                    m_mixBuffer[f * channels + c] = std::clamp(
                        m_mixBuffer[f * channels + c], -1.0f, 1.0f);
                }
            }
        } else {
            // Same sample rate — read and mix directly
            if (m_micReadBuffer.size() < micSamplesNeeded) {
                m_micReadBuffer.resize(micSamplesNeeded);
            }
            size_t read = m_micRingBuffer.Read(m_micReadBuffer.data(), micSamplesNeeded);
            uint32_t micFramesRead = static_cast<uint32_t>(read / std::max(m_micChannels, 1));

            for (uint32_t f = 0; f < std::min(frameCount, micFramesRead); f++) {
                for (int c = 0; c < channels; c++) {
                    int micCh = (m_micChannels == 1) ? 0 : std::min(c, m_micChannels - 1);
                    m_mixBuffer[f * channels + c] += m_micReadBuffer[f * m_micChannels + micCh];
                    m_mixBuffer[f * channels + c] = std::clamp(
                        m_mixBuffer[f * channels + c], -1.0f, 1.0f);
                }
            }
        }

        m_wavWriter.WriteSamples(m_mixBuffer.data(), frameCount);
    } else {
        // No mic — write loopback data directly
        m_wavWriter.WriteSamples(data, frameCount);
    }
}

bool AudioCaptureManager::StartMicCapture() {
    StopMicCapture();

    m_micContext = new ma_context;
    if (ma_context_init(nullptr, 0, nullptr, m_micContext) != MA_SUCCESS) {
        delete m_micContext;
        m_micContext = nullptr;
        return false;
    }

    m_micDevice = new ma_device;
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = 1; // mono mic
    config.sampleRate = static_cast<ma_uint32>(m_outputSampleRate); // try to match output

    // Set up callback context
    s_micCallbackCtx.channels = 1;

    // Size ring buffer for ~500ms of audio
    size_t ringSize = static_cast<size_t>(m_outputSampleRate) * 1; // 1 channel * 0.5s, rounded up
    m_micRingBuffer.Resize(ringSize);
    s_micCallbackCtx.ringBuffer = &m_micRingBuffer;

    config.dataCallback = micCallbackWrapper;
    config.pUserData = &s_micCallbackCtx;

    if (ma_device_init(m_micContext, &config, m_micDevice) != MA_SUCCESS) {
        delete m_micDevice;
        m_micDevice = nullptr;
        ma_context_uninit(m_micContext);
        delete m_micContext;
        m_micContext = nullptr;
        return false;
    }

    // Record the actual mic format (may differ from requested)
    m_micSampleRate = static_cast<int>(m_micDevice->sampleRate);
    m_micChannels = static_cast<int>(m_micDevice->capture.channels);
    s_micCallbackCtx.channels = m_micChannels;

    if (ma_device_start(m_micDevice) != MA_SUCCESS) {
        ma_device_uninit(m_micDevice);
        delete m_micDevice;
        m_micDevice = nullptr;
        ma_context_uninit(m_micContext);
        delete m_micContext;
        m_micContext = nullptr;
        return false;
    }

    return true;
}

void AudioCaptureManager::StopMicCapture() {
    if (m_micDevice) {
        ma_device_stop(m_micDevice);
        ma_device_uninit(m_micDevice);
        delete m_micDevice;
        m_micDevice = nullptr;
    }
    if (m_micContext) {
        ma_context_uninit(m_micContext);
        delete m_micContext;
        m_micContext = nullptr;
    }
    m_micRingBuffer.Clear();
}

std::string AudioCaptureManager::BuildFilePath(const std::string& replayId, int segmentNumber) {
    // Build filename: sc_<id>_seg000.wav
    std::string id = replayId;

    // Strip known prefixes to avoid doubling
    for (const auto& prefix : {"synccomms_", "sc_"}) {
        if (id.size() > strlen(prefix) && id.substr(0, strlen(prefix)) == prefix) {
            id = id.substr(strlen(prefix));
            break;
        }
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "sc_%s_seg%03d.wav", id.c_str(), segmentNumber);

    return m_config->GetOutputDir() + buf;
}

} // namespace SyncComms
