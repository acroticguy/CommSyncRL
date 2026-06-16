#pragma once

#include "SyncComms/Config.h"
#include "SyncComms/StandaloneConfig.h"
#include "SyncComms/SyncState.h"
#include "SyncComms/AudioCaptureManager.h"
#include "SyncComms/SidecarManager.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace SyncComms {

enum class CapturePhase {
    Idle,
    Armed,
    Recording,
    Paused,
    Finalizing,
    Error,
};

struct CaptureStatus {
    CapturePhase phase = CapturePhase::Idle;
    std::string  matchGuid;        // empty when phase == Idle
    int          segmentCount = 0;
    double       segmentSeconds = 0.0;
    std::string  lastError;
};

/// Drives the audio capture pipeline from the Stats API event stream.
///
/// On the wire we consume the normalized event shape `{event, data}`. Lifecycle
/// mapping mirrors what the BakkesMod plugin used to do via direct hooks:
///
///   CountdownBegin / MatchCreated  → arm + start a new segment
///   StatfeedEvent[Goal]            → close current segment, start a new one
///   MatchPaused / MatchUnpaused    → pause/resume capture
///   MatchEnded                     → finalize current segment, compress, write sidecar
///   MatchDestroyed (no MatchEnded) → discard current capture
///
/// Settings come from a StandaloneConfig that the host updates from the
/// SettingsStore JSON; we re-read on each match start so changes between
/// matches take effect.
class CaptureOrchestrator {
public:
    using StatusCallback = std::function<void(const CaptureStatus&)>;

    CaptureOrchestrator(StandaloneConfig& config);
    ~CaptureOrchestrator();

    CaptureOrchestrator(const CaptureOrchestrator&) = delete;
    CaptureOrchestrator& operator=(const CaptureOrchestrator&) = delete;

    /// Feed a normalized Stats API event (event name + data object).
    /// `dataJson` is the raw JSON of the data sub-object, parsed by the caller
    /// or passed through. Safe to call from the StatsApiClient worker thread.
    void HandleEvent(const std::string& eventName, const std::string& dataJson);

    /// Force-stop any in-progress capture (called on app shutdown).
    void Shutdown();

    void SetStatusCallback(StatusCallback cb) {
        std::lock_guard<std::mutex> lock(m_cbMutex);
        m_onStatus = std::move(cb);
    }

    CaptureStatus GetStatus() const;

private:
    void StartNewMatch(const std::string& matchGuid);
    void StartNewSegment();
    void CloseCurrentSegment(int endFrame);
    void Finalize();
    void Reset();
    void EmitStatus();

    /// Spawn a detached worker that polls the demos directory for the
    /// just-saved .replay file (RL writes it shortly after MatchEnded),
    /// parses its goalFrames, anchors the captured segment times, and
    /// rewrites the sidecar with `anchored=true` + the .replay path.
    /// PlaybackController then skips its own anchoring on EnterReplay,
    /// avoiding the directory rescan + binary header parse that would
    /// otherwise stutter on replay open.
    void StartPostMatchLink(std::string matchGuid,
                             std::vector<SegmentInfo> segments,
                             int64_t matchStartEpoch);

    StandaloneConfig& m_config;
    std::shared_ptr<SyncState> m_syncState;
    std::unique_ptr<AudioCaptureManager> m_capture;
    std::unique_ptr<SidecarManager> m_sidecar;

    mutable std::mutex m_mutex;
    CaptureStatus m_status;
    std::vector<SegmentInfo> m_segments;
    std::string m_currentMatchGuid;
    int m_currentSegmentIndex = 0;
    int m_lastFrame = 0;
    /// Wall-clock time of the very first UpdateState we see for the current
    /// match. We treat "seconds since this point" as our time-base. (The Stats
    /// API does NOT emit Frame/Elapsed during a live match — those fields
    /// only appear during replay viewing — so we can't rely on them here.)
    double m_matchStartWall = 0.0;
    /// Unix seconds at the first UpdateState of the match (system clock, not
    /// the steady clock m_matchStartWall uses). Persisted to the sidecar and
    /// matched against each .replay header's MatchStartEpoch to locate the
    /// file — RL replay headers have no MatchGuid to join on.
    int64_t m_matchStartSystemEpoch = 0;
    /// Seconds since match start at the moment of the most recent UpdateState.
    /// This is what gets written into segment start/end so playback (which
    /// converts `Frame * frameTime` to seconds-since-replay-start) can find
    /// the right segment.
    double m_lastElapsed = 0.0;
    double m_segmentStartElapsed = 0.0;
    double m_segmentStartWall = 0.0;
    /// Capture-clock time of the most recent GoalScored while Recording, or
    /// -1 when no goal is pending. Consumed by CloseCurrentSegment so the
    /// segment carries the exact goal moment (GoalReplayStart, which closes
    /// the segment, fires several seconds after the goal — celebration time
    /// we want in the audio but NOT in the anchor point).
    double m_pendingGoalElapsed = -1.0;

    /// Diagnostic: raw JSON of the most recent UpdateState we received, so
    /// the F12 console can inspect the actual wire shape during a live match.
    std::string m_lastUpdateStateRaw;
public:
    std::string GetLastUpdateStateRaw() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastUpdateStateRaw;
    }

    // Latest bReplay observation from UpdateState. -1 = unknown (haven't
    // seen any state sample yet for the current match), 0 = live match,
    // 1 = replay viewer. Used to gate StartNewSegment so we never record
    // during replay viewing — the API re-fires MatchCreated/CountdownBegin
    // when a saved replay opens, which would otherwise trigger capture.
    int m_lastBReplay = -1;

    // Raw bReplay from the last UpdateState, WITHOUT the Frame-presence
    // heuristic above (so in-match cinematics read as 1 here, 0 in
    // m_lastBReplay). -1 = not yet observed. Two uses, both validated
    // against recorded wire data (tools/stats_api_raw_*.jsonl):
    //  - the 0->1 flip is the segment-close boundary (the documented
    //    GoalReplayStart/End events are NOT emitted on current RL builds);
    //  - GoalScored re-fires during the cinematic with an empty payload
    //    (GoalTime=0, no Scorer); stamping is gated on raw bReplay == 0 so
    //    only the real, live goal sets m_pendingGoalElapsed.
    int m_lastRawBReplay = -1;

    // Set when raw bReplay flips 0->1 (cinematic started); consumed on the
    // same UpdateState after m_lastElapsed is refreshed, closing the
    // current segment at the cinematic boundary.
    bool m_cinematicJustStarted = false;

    /// CountdownBegin can arrive before the first UpdateState, in which case
    /// we don't yet know whether this is a live match or a saved replay
    /// playback re-firing lifecycle events. Defer the segment start until the
    /// next UpdateState confirms bReplay=0; then consume the flag.
    bool m_pendingSegmentStart = false;

    mutable std::mutex m_cbMutex;
    StatusCallback m_onStatus;
};

} // namespace SyncComms
