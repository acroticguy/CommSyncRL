#pragma once

#include "SyncComms/SyncState.h"
#include "SyncComms/Config.h"
#include <memory>
#include <vector>
#include <string>
#include <atomic>

struct ma_device;
struct ma_context;
struct ma_decoder;

namespace SyncComms {

/// Time-locked audio playback manager.
/// Replay elapsed time drives everything — which segment plays and the audio position.
/// No dependency on game events for playback control.
class AudioPlaybackManager {
public:
    AudioPlaybackManager(std::shared_ptr<SyncState> syncState, Config* config);
    ~AudioPlaybackManager();

    AudioPlaybackManager(const AudioPlaybackManager&) = delete;
    AudioPlaybackManager& operator=(const AudioPlaybackManager&) = delete;

    /// Load segment metadata (called when replay opens).
    void LoadSegments(const std::vector<SegmentInfo>& segments);

    /// Called every game tick during replay viewing.
    /// Determines which segment should play and keeps audio locked to replay time.
    void SyncToReplayTime(float replayTimeSec);

    /// Stop all playback and release resources.
    void StopPlayback();

    /// Called from miniaudio's audio thread — fills the output buffer.
    void OnPlaybackData(float* output, uint32_t frameCount);

private:
    bool InitPlaybackDevice();
    void DestroyPlaybackDevice();
    bool OpenDecoder(const std::string& audioFile);
    bool OpenDecoderFromMemory(const std::vector<uint8_t>& oggData);
    void CloseDecoder();

    /// Find which segment index should be playing at the given replay time. Returns -1 if none.
    int FindSegmentForTime(float replayTimeSec) const;

    std::shared_ptr<SyncState>  m_syncState;
    Config*                     m_config;

    // miniaudio
    ma_device*   m_device   = nullptr;
    ma_context*  m_context  = nullptr;
    ma_decoder*  m_decoder  = nullptr;

    // Segments
    std::vector<SegmentInfo>    m_segments;
    std::string                 m_outputDir;

    // Playback state
    int           m_sampleRate      = 48000;
    int           m_channels        = 2;
    bool          m_deviceReady     = false;
    int           m_currentSegIdx   = -1;
    std::atomic<bool> m_playing{false};

    // Time-lock state
    float         m_segmentStartReplayTime = 0.0f;
    std::atomic<int64_t> m_targetSample{0};
    int64_t       m_decoderPosition = 0;

    // Memory buffer for embedded audio (must outlive decoder)
    std::vector<uint8_t> m_decoderBuffer;

    // Pause detection
    float         m_lastReplayTime  = -1.0f;
    int           m_staleTicks      = 0;
    std::atomic<bool> m_paused{false};
};

} // namespace SyncComms
