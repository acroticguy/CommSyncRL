#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <cstdint>

namespace SyncComms {

struct SegmentInfo {
    int         index;
    int         startFrame;
    int         endFrame;
    double      startTimeSec;   // server.GetSecondsElapsed() at capture start
    double      endTimeSec;     // server.GetSecondsElapsed() at capture end
    double      frameTime;      // seconds per replay frame (typically 1/30)
    std::string audioFile;      // relative path to audio file (WAV or OGG)
    std::string audioData;      // base64-encoded OGG data (embedded in JSON)
    std::string event;          // human-readable label (e.g. "kickoff_to_goal")
};

/// Thread-safe shared state between game thread and audio threads.
/// Atomics for hot-path data; mutex only for segment list changes.
class SyncState {
public:
    // === Lock-free atomics (hot path) ===
    // Written by game thread, read by audio threads
    std::atomic<int>    currentReplayFrame{0};
    std::atomic<float>  timeDilation{1.0f};
    std::atomic<bool>   isReplayPaused{false};
    std::atomic<bool>   isInReplay{false};
    std::atomic<bool>   isRecording{false};
    std::atomic<bool>   isCapturingSegment{false};

    // Seek signaling: game thread sets both, audio thread clears requestSeek
    std::atomic<bool>   requestSeek{false};
    std::atomic<int>    seekTargetFrame{0};

    // UI-driven parameters
    std::atomic<float>  latencyOffsetMs{0.0f};
    std::atomic<float>  volumeMultiplier{1.0f};

    // Active segment index — written by game thread, read by audio thread
    std::atomic<int>    activeSegmentIndex{-1};

    // Status reporting (audio thread -> game thread, for UI display)
    std::atomic<int64_t> currentAudioSample{0};
    std::atomic<float>   currentDriftMs{0.0f};

    // === Mutex-protected (cold path: segment transitions only) ===
    void SetSegments(const std::vector<SegmentInfo>& segs);
    std::vector<SegmentInfo> GetSegments() const;
    SegmentInfo GetSegment(int index) const;
    int GetSegmentCount() const;

    void Reset();

private:
    mutable std::mutex       m_segmentMutex;
    std::vector<SegmentInfo> m_segments;
};

} // namespace SyncComms
