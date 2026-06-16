#pragma once

#include "SyncComms/Config.h"

#include <atomic>
#include <mutex>
#include <string>

namespace SyncComms {

/// Config implementation backed by the standalone app's in-memory settings
/// state. Thread-safe — the audio engine reads on its capture/playback threads
/// while the UI thread writes from the SettingsStore JSON or live UI changes.
class StandaloneConfig : public Config {
public:
    StandaloneConfig();

    bool        IsEnabled() const override;
    float       GetVolume() const override;
    float       GetLatencyOffsetMs() const override;
    std::string GetCaptureSource() const override;
    std::string GetTargetProcessName() const override;
    bool        GetIncludeMic() const override;
    std::string GetOutputDir() const override;
    int         GetSampleRate() const override;
    int         GetChannels() const override;

    // ── Setters used by the orchestrator/main when settings JSON changes ───
    void SetEnabled(bool v)                        { m_enabled.store(v); }
    void SetVolume(float v)                        { m_volume.store(v); }
    void SetLatencyOffsetMs(float v)               { m_latencyOffsetMs.store(v); }
    void SetIncludeMic(bool v)                     { m_includeMic.store(v); }
    void SetSampleRate(int v)                      { m_sampleRate.store(v); }
    void SetChannels(int v)                        { m_channels.store(v); }

    void SetTargetProcessName(std::string v) {
        std::lock_guard<std::mutex> lock(m_strMutex);
        m_targetProcess = std::move(v);
    }
    void SetOutputDir(std::string v) {
        std::lock_guard<std::mutex> lock(m_strMutex);
        m_outputDir = std::move(v);
    }
    void SetMicrophoneId(std::string v) {
        std::lock_guard<std::mutex> lock(m_strMutex);
        m_microphoneId = std::move(v);
    }
    /// Stable PnP id of the user-selected microphone (empty = system default).
    std::string GetMicrophoneId() const {
        std::lock_guard<std::mutex> lock(m_strMutex);
        return m_microphoneId;
    }

private:
    std::atomic<bool>  m_enabled{true};
    std::atomic<float> m_volume{1.0f};
    // Default +100ms: the captured audio lands ~100ms early vs the replay
    // picture, so delay it by default. Positive = play later. Overridable via
    // settings.json ("latencyOffsetMs").
    std::atomic<float> m_latencyOffsetMs{100.0f};
    std::atomic<bool>  m_includeMic{false};
    std::atomic<int>   m_sampleRate{48000};
    std::atomic<int>   m_channels{1};

    mutable std::mutex m_strMutex;
    std::string m_targetProcess; // empty = full system loopback
    std::string m_outputDir;     // resolved at startup, trailing slash
    std::string m_microphoneId;  // empty = default
};

} // namespace SyncComms
