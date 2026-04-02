#include "SyncComms/AudioPlaybackManager.h"
#include "SyncComms/AudioCompressor.h"
#include "miniaudio.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>

namespace SyncComms {

static void playbackCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    auto* mgr = static_cast<AudioPlaybackManager*>(pDevice->pUserData);
    mgr->OnPlaybackData(static_cast<float*>(pOutput), frameCount);
}

AudioPlaybackManager::AudioPlaybackManager(std::shared_ptr<SyncState> syncState, Config* config)
    : m_syncState(std::move(syncState))
    , m_config(config)
    , m_sampleRate(config->GetSampleRate())
    , m_channels(config->GetChannels())
{
}

AudioPlaybackManager::~AudioPlaybackManager() {
    StopPlayback();
}

void AudioPlaybackManager::LoadSegments(const std::vector<SegmentInfo>& segments) {
    m_segments = segments;
    m_outputDir = m_config->GetOutputDir();
    m_currentSegIdx = -1;

    if (!m_deviceReady) {
        if (InitPlaybackDevice()) {
            if (ma_device_start(m_device) == MA_SUCCESS) {
                m_deviceReady = true;
            } else {
                DestroyPlaybackDevice();
            }
        }
    }
}

int AudioPlaybackManager::FindSegmentForTime(float replayTimeSec) const {
    for (int i = 0; i < static_cast<int>(m_segments.size()); i++) {
        const auto& seg = m_segments[i];
        float start = static_cast<float>(seg.startTimeSec);
        float end = static_cast<float>(seg.endTimeSec);
        if (start > 0.0f && end > 0.0f) {
            if (replayTimeSec >= start - 0.2f && replayTimeSec <= end + 0.2f) {
                return i;
            }
        }
    }
    return -1;
}

void AudioPlaybackManager::SyncToReplayTime(float replayTimeSec) {
    // Pause detection: if replay time hasn't changed, we're paused
    if (std::abs(replayTimeSec - m_lastReplayTime) < 0.001f) {
        m_staleTicks++;
        if (m_staleTicks > 30) {
            m_paused.store(true);
        }
    } else {
        m_staleTicks = 0;
        m_paused.store(false);
    }
    m_lastReplayTime = replayTimeSec;

    // Determine which segment should be playing right now
    int shouldPlay = FindSegmentForTime(replayTimeSec);

    if (shouldPlay != m_currentSegIdx) {
        // Segment changed — switch
        m_playing.store(false);
        CloseDecoder();
        m_currentSegIdx = -1;
        m_syncState->activeSegmentIndex.store(-1);

        if (shouldPlay >= 0) {
            const auto& seg = m_segments[shouldPlay];
            bool opened = false;

            // Try embedded audio first, fall back to file
            if (!seg.audioData.empty()) {
                auto oggData = AudioCompressor::Base64Decode(seg.audioData);
                opened = OpenDecoderFromMemory(oggData);
            }
            if (!opened && !seg.audioFile.empty()) {
                std::string fullPath = m_outputDir + seg.audioFile;
                opened = OpenDecoder(fullPath);
            }

            if (opened) {
                m_currentSegIdx = shouldPlay;
                m_segmentStartReplayTime = static_cast<float>(seg.startTimeSec);
                m_syncState->activeSegmentIndex.store(shouldPlay);

                // Seek to the right position within the segment
                float elapsed = replayTimeSec - m_segmentStartReplayTime;
                if (elapsed < 0.0f) elapsed = 0.0f;
                int64_t target = static_cast<int64_t>(elapsed * m_sampleRate);
                ma_decoder_seek_to_pcm_frame(m_decoder, static_cast<ma_uint64>(target));
                m_decoderPosition = target;
                m_targetSample.store(target);

                m_playing.store(true);
            }
        }
    } else if (m_playing.load()) {
        // Same segment — update target sample for time-lock
        float elapsed = replayTimeSec - m_segmentStartReplayTime;
        if (elapsed < 0.0f) elapsed = 0.0f;
        int64_t target = static_cast<int64_t>(elapsed * m_sampleRate);
        m_targetSample.store(target, std::memory_order_relaxed);
    }
}

void AudioPlaybackManager::StopPlayback() {
    m_playing.store(false);
    CloseDecoder();
    m_currentSegIdx = -1;
    m_syncState->activeSegmentIndex.store(-1);
    if (m_device) {
        ma_device_stop(m_device);
    }
    DestroyPlaybackDevice();
    m_deviceReady = false;
    m_lastReplayTime = -1.0f;
    m_staleTicks = 0;
}

void AudioPlaybackManager::OnPlaybackData(float* output, uint32_t frameCount) {
    std::memset(output, 0, frameCount * m_channels * sizeof(float));

    if (!m_playing.load(std::memory_order_relaxed) || !m_decoder) {
        return;
    }

    // If paused, output silence
    if (m_paused.load(std::memory_order_relaxed)) {
        return;
    }

    float volume = m_syncState->volumeMultiplier.load(std::memory_order_relaxed);

    // Check if we need to seek (user scrubbed)
    int64_t target = m_targetSample.load(std::memory_order_relaxed);
    int64_t drift = target - m_decoderPosition;

    if (std::abs(drift) > m_sampleRate / 10) { // >100ms drift = seek
        if (target >= 0) {
            ma_decoder_seek_to_pcm_frame(m_decoder, static_cast<ma_uint64>(target));
            m_decoderPosition = target;
        }
    }

    // Read audio
    ma_uint64 framesRead = 0;
    ma_decoder_read_pcm_frames(m_decoder, output, frameCount, &framesRead);
    m_decoderPosition += static_cast<int64_t>(framesRead);

    // Apply volume
    uint32_t sampleCount = static_cast<uint32_t>(framesRead) * m_channels;
    for (uint32_t i = 0; i < sampleCount; i++) {
        output[i] *= volume;
    }

    // End of file — stop this segment
    if (framesRead < frameCount) {
        m_playing.store(false);
    }
}

bool AudioPlaybackManager::InitPlaybackDevice() {
    DestroyPlaybackDevice();

    m_context = new ma_context;
    if (ma_context_init(nullptr, 0, nullptr, m_context) != MA_SUCCESS) {
        delete m_context;
        m_context = nullptr;
        return false;
    }

    m_device = new ma_device;
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(m_channels);
    config.sampleRate = static_cast<ma_uint32>(m_sampleRate);
    config.dataCallback = playbackCallback;
    config.pUserData = this;

    if (ma_device_init(m_context, &config, m_device) != MA_SUCCESS) {
        delete m_device;
        m_device = nullptr;
        ma_context_uninit(m_context);
        delete m_context;
        m_context = nullptr;
        return false;
    }

    return true;
}

void AudioPlaybackManager::DestroyPlaybackDevice() {
    if (m_device) {
        ma_device_uninit(m_device);
        delete m_device;
        m_device = nullptr;
    }
    if (m_context) {
        ma_context_uninit(m_context);
        delete m_context;
        m_context = nullptr;
    }
}

bool AudioPlaybackManager::OpenDecoder(const std::string& audioFile) {
    CloseDecoder();

    if (!std::filesystem::exists(audioFile)) return false;

    m_decoder = new ma_decoder;

    // Let miniaudio auto-detect format; only force float32 output
    ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, 0);

    if (ma_decoder_init_file(audioFile.c_str(), &decoderConfig, m_decoder) != MA_SUCCESS) {
        delete m_decoder;
        m_decoder = nullptr;
        return false;
    }

    // Update our playback params to match what the decoder actually outputs
    m_sampleRate = static_cast<int>(m_decoder->outputSampleRate);
    m_channels = static_cast<int>(m_decoder->outputChannels);

    m_decoderPosition = 0;
    return true;
}

bool AudioPlaybackManager::OpenDecoderFromMemory(const std::vector<uint8_t>& oggData) {
    CloseDecoder();
    if (oggData.empty()) return false;

    m_decoderBuffer = oggData;

    m_decoder = new ma_decoder;
    ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, 0);

    if (ma_decoder_init_memory(m_decoderBuffer.data(), m_decoderBuffer.size(),
                                &decoderConfig, m_decoder) != MA_SUCCESS) {
        delete m_decoder;
        m_decoder = nullptr;
        m_decoderBuffer.clear();
        return false;
    }

    m_sampleRate = static_cast<int>(m_decoder->outputSampleRate);
    m_channels = static_cast<int>(m_decoder->outputChannels);
    m_decoderPosition = 0;
    return true;
}

void AudioPlaybackManager::CloseDecoder() {
    if (m_decoder) {
        ma_decoder_uninit(m_decoder);
        delete m_decoder;
        m_decoder = nullptr;
    }
    m_decoderBuffer.clear();
    m_decoderPosition = 0;
}

} // namespace SyncComms
