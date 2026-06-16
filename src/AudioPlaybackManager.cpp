#include "SyncComms/AudioPlaybackManager.h"
#include "SyncComms/AudioCompressor.h"
#include "miniaudio.h"
#include <chrono>
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
    // Don't init the playback device here — we need to know the captured
    // OGG's actual sample rate and channel count first. Device is initialized
    // lazily in SyncToReplayTime once a decoder is opened, so its format
    // matches the audio it'll be playing. (Capture's WASAPI side picks the
    // hardware's natural format, often 48kHz/2ch — Config::GetChannels()
    // defaults to 1 and would produce a buffer-size mismatch if used here.)
}

int AudioPlaybackManager::FindSegmentForTime(float replayTimeSec) const {
    for (int i = 0; i < static_cast<int>(m_segments.size()); i++) {
        const auto& seg = m_segments[i];
        float start = static_cast<float>(seg.startTimeSec);
        float end = static_cast<float>(seg.endTimeSec);
        // A valid segment just needs positive width. start may be <= 0: per-goal
        // anchoring can place the FIRST segment's window a hair before replay
        // frame 0 (e.g. start=-0.39s), since the round began at the very start
        // of the recording. The old `start > 0` guard silently dropped that
        // first segment, so it never played ("between segments" forever).
        if (end > start) {
            if (replayTimeSec >= start - 0.2f && replayTimeSec <= end + 0.2f) {
                return i;
            }
        }
    }
    return -1;
}

void AudioPlaybackManager::SyncToReplayTime(float replayTimeSec) {
    // Pause detection: if replay time hasn't progressed for ~300ms of wall
    // time, the replay is paused. Time-based so 30Hz and 120Hz callers
    // silence equally fast.
    const double nowSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    m_paused.store(m_stallDetector.Update(replayTimeSec, nowSec));

    // Constant A/V latency trim. POSITIVE = play audio LATER (delay it): we
    // read from an earlier point in the segment for a given replay time, so the
    // sound lands behind the picture. Read live from config each tick
    // (StandaloneConfig defaults to +100ms; the BakkesMod build defaults to 0).
    // Segment SELECTION stays on the raw replay time — 100ms is well within the
    // ±0.2s window tolerance, so the trim only shifts the audio read position.
    const float latencySec = m_config->GetLatencyOffsetMs() / 1000.0f;

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

                // First decoder open of this playback session — initialize
                // the device with the decoder's actual sample rate / channel
                // count. (OpenDecoder*() updated m_sampleRate and m_channels
                // from the decoder's outputs.)
                if (!m_deviceReady) {
                    if (InitPlaybackDevice() &&
                        ma_device_start(m_device) == MA_SUCCESS) {
                        m_deviceReady = true;
                    } else {
                        DestroyPlaybackDevice();
                        CloseDecoder();
                        m_currentSegIdx = -1;
                        return;
                    }
                }

                // Seek to the right position within the segment
                float elapsed = replayTimeSec - m_segmentStartReplayTime - latencySec;
                if (elapsed < 0.0f) elapsed = 0.0f;
                int64_t target = static_cast<int64_t>(elapsed * m_sampleRate);
                ma_decoder_seek_to_pcm_frame(m_decoder, static_cast<ma_uint64>(target));
                m_decoderPosition = target;
                m_targetSample.store(target);

                m_playing.store(true);
            }
        }
    } else if (m_decoder) {
        // Same segment, decoder still open — keep target locked to replay
        // time even if we previously hit EOF (m_playing == false). On a
        // backward scrub, target < m_decoderPosition; re-arming m_playing
        // lets OnPlaybackData's drift-seek path jump the decoder back.
        float elapsed = replayTimeSec - m_segmentStartReplayTime - latencySec;
        if (elapsed < 0.0f) elapsed = 0.0f;
        int64_t target = static_cast<int64_t>(elapsed * m_sampleRate);
        m_targetSample.store(target, std::memory_order_relaxed);
        if (!m_playing.load(std::memory_order_relaxed)) {
            m_playing.store(true, std::memory_order_relaxed);
        }
    }
}

void AudioPlaybackManager::SetSegmentStart(int idx, double startTimeSec) {
    if (idx < 0 || idx >= static_cast<int>(m_segments.size())) return;
    const float prevStart = static_cast<float>(m_segments[idx].startTimeSec);
    const float newStart  = static_cast<float>(startTimeSec);
    // Width is preserved relative to the existing endTimeSec — this is a
    // start-only update, intended for snapping a not-yet-played segment to
    // its observed kickoff frame.
    m_segments[idx].startTimeSec = startTimeSec;
    if (idx == m_currentSegIdx && std::abs(prevStart - newStart) > 0.001f) {
        // We're already inside this segment when the kickoff event lands.
        // Update the in-flight reference; the next SyncToReplayTime tick
        // will recompute m_targetSample against the new origin and the
        // audio thread's drift-seek will catch up.
        m_segmentStartReplayTime = newStart;
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
    m_stallDetector.Reset();
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
        ma_uint64 totalFrames = 0;
        bool haveLength = ma_decoder_get_length_in_pcm_frames(m_decoder, &totalFrames) == MA_SUCCESS;
        if (target >= 0 && (!haveLength || static_cast<ma_uint64>(target) < totalFrames)) {
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
