#pragma once

#include "SyncComms/StandaloneConfig.h"
#include "SyncComms/SyncState.h"
#include "SyncComms/AudioPlaybackManager.h"
#include "SyncComms/SidecarManager.h"
#include "SyncComms/ReplayLocator.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace SyncComms {

enum class PlaybackPhase {
    Idle,           // not in a replay viewer
    SearchingSidecar,
    NoSidecar,      // replay viewer is open but we have no audio for this match
    Playing,        // replay viewer + audio synced
    Error,
};

struct PlaybackStatus {
    PlaybackPhase phase = PlaybackPhase::Idle;
    std::string   matchGuid;        // empty = no current replay
    std::string   sidecarPath;      // empty if no sidecar
    std::string   replayPath;       // empty if .replay not located
    int           segmentCount = 0;
    /// Index of the segment whose audio is currently playing, or -1 if none
    /// (between segments / before first / after last). Mirrors
    /// SyncState::activeSegmentIndex for UI consumption.
    int           activeSegmentIndex = -1;
    double        elapsedSec = 0.0;
    bool          anchored = false; // true = segments aligned via .replay metadata
    std::string   lastError;
};

/// Subscribes to the Stats API event stream. Whenever an UpdateState arrives
/// with bReplay=true, we find the sidecar for that MatchGuid and drive
/// AudioPlaybackManager::SyncToReplayTime(Frame * frameTime). (Frame, not
/// Elapsed — Elapsed is the in-game clock and freezes during cinematics and
/// countdowns.) Mirrors what the BakkesMod plugin did via
/// GameViewportClient.Tick + ReplayServerWrapper.
class PlaybackController {
public:
    using StatusCallback = std::function<void(const PlaybackStatus&)>;

    PlaybackController(StandaloneConfig& config);
    ~PlaybackController();

    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    void HandleEvent(const std::string& eventName, const std::string& dataJson);
    void Shutdown();

    void SetStatusCallback(StatusCallback cb) {
        std::lock_guard<std::mutex> lock(m_cbMutex);
        m_onStatus = std::move(cb);
    }
    PlaybackStatus GetStatus() const;

private:
    void EnterReplay(const std::string& matchGuid);
    void ExitReplay();
    void EmitStatus();

    StandaloneConfig& m_config;
    std::shared_ptr<SyncState> m_syncState;
    std::unique_ptr<AudioPlaybackManager> m_playback;
    std::unique_ptr<SidecarManager> m_sidecar;
    std::unique_ptr<ReplayLocator> m_replayLocator;

    mutable std::mutex m_mutex;
    PlaybackStatus m_status;
    bool m_inReplay = false;
    /// True once we've successfully aligned segments to the canonical replay
    /// timeline using `.replay` metadata (goal frames + total frame count).
    /// When set, Frame * m_frameTime from UpdateState is used directly. When
    /// false, we fall back to the elapsedFloor calibration heuristic below.
    bool   m_anchored = false;
    /// Seconds per replay frame for the current replay (1 / RecordFPS from
    /// the .replay header, or the sidecar's stored frameTime on the
    /// pre-anchored fast path). Reset to 1/30 on ExitReplay.
    double m_frameTime = 1.0 / 30.0;
    /// Calibration fallback for when anchoring fails (no `.replay` found, parse
    /// error, or segment-count mismatch). Smallest `Elapsed` we've seen this
    /// replay viewing session — captured ONCE, never updated, so a backward
    /// scrub doesn't re-baseline and warp the lookup time to ~0.
    double m_elapsedFloor = 0.0;
    bool   m_haveElapsedFloor = false;

    /// Diagnostics: throttle per-tick logging to ~1 Hz so the user can read
    /// off Frame at the visual goal moment. Reset on ExitReplay.
    int    m_lastLoggedFrame = -1;
    int    m_lastLoggedSegIdx = -2;

    mutable std::mutex m_cbMutex;
    StatusCallback m_onStatus;
};

} // namespace SyncComms
