#pragma once

#include "SyncComms/SyncState.h"
#include "SyncComms/Config.h"
#include "SyncComms/WavWriter.h"
#include "SyncComms/WasapiCapture.h"
#include "SyncComms/RingBuffer.h"
#include "SyncComms/AudioResampler.h"
#include <memory>
#include <string>
#include <mutex>
#include <vector>

// Forward declare miniaudio types
struct ma_device;
struct ma_context;

namespace SyncComms {

class AudioCaptureManager {
public:
    AudioCaptureManager(std::shared_ptr<SyncState> syncState, Config* config);
    ~AudioCaptureManager();

    AudioCaptureManager(const AudioCaptureManager&) = delete;
    AudioCaptureManager& operator=(const AudioCaptureManager&) = delete;

    /// Start capturing audio for a new segment.
    void StartCapture(const std::string& replayId, int segmentNumber, int startFrame);

    /// Stop the current capture and return the completed segment info.
    SegmentInfo StopCapture(int endFrame);

    /// Whether the capture device is currently running.
    bool IsCapturing() const;

private:
    // WASAPI capture data callback
    void OnWasapiData(const float* data, uint32_t frameCount, int channels);

    // Mic helpers
    bool StartMicCapture();
    void StopMicCapture();

    std::string BuildFilePath(const std::string& replayId, int segmentNumber);

    std::shared_ptr<SyncState> m_syncState;
    Config*                    m_config;

    // WASAPI per-process (or full loopback) capture
    std::unique_ptr<WasapiCapture> m_wasapiCapture;

    // Microphone capture (miniaudio, optional)
    ma_device*  m_micDevice  = nullptr;
    ma_context* m_micContext = nullptr;
    RingBuffer  m_micRingBuffer;
    AudioResampler m_micResampler;
    int         m_micSampleRate = 0;
    int         m_micChannels   = 0;

    // Output
    WavWriter   m_wavWriter;
    int         m_outputChannels  = 2;   // matches WASAPI actual channels
    int         m_outputSampleRate = 48000;

    // Current segment state
    std::string m_currentReplayId;
    int         m_currentSegment  = 0;
    int         m_startFrame      = 0;
    bool        m_capturing       = false;
    std::mutex  m_captureMutex;

    // Temp buffers for mixing
    std::vector<float> m_mixBuffer;
    std::vector<float> m_micReadBuffer;
    std::vector<float> m_micResampleBuffer;
};

} // namespace SyncComms
