#include "SyncComms/PlaybackController.h"
#include "SyncComms/ReplayMetadata.h"
#include "SyncComms/SegmentAnchoring.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace SyncComms {

PlaybackController::PlaybackController(StandaloneConfig& config)
    : m_config(config)
    , m_syncState(std::make_shared<SyncState>())
{
    auto log = [](const std::string& m) {
        std::cerr << "[PlaybackController] " << m << std::endl;
    };
    m_playback       = std::make_unique<AudioPlaybackManager>(m_syncState, &m_config);
    m_sidecar        = std::make_unique<SidecarManager>(&m_config, log);
    m_replayLocator  = std::make_unique<ReplayLocator>();
}

PlaybackController::~PlaybackController() {
    Shutdown();
}

void PlaybackController::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_playback) m_playback->StopPlayback();
    m_inReplay = false;
    m_status = PlaybackStatus{};
    EmitStatus();
}

PlaybackStatus PlaybackController::GetStatus() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void PlaybackController::EmitStatus() {
    PlaybackStatus snap = m_status;
    StatusCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_cbMutex);
        cb = m_onStatus;
    }
    if (cb) cb(snap);
}

void PlaybackController::EnterReplay(const std::string& matchGuid) {
    m_status.matchGuid = matchGuid;
    m_status.phase = PlaybackPhase::SearchingSidecar;
    m_status.replayPath.clear();
    m_status.anchored = false;
    m_status.lastError.clear();
    EmitStatus();

    auto path = m_sidecar->FindSidecar(matchGuid);
    if (!path) {
        m_status.phase = PlaybackPhase::NoSidecar;
        m_status.sidecarPath.clear();
        m_status.segmentCount = 0;
        EmitStatus();
        return;
    }

    auto sc = m_sidecar->ReadSidecar(*path);
    auto& segs = sc.segments;
    if (segs.empty()) {
        m_status.phase = PlaybackPhase::NoSidecar;
        m_status.sidecarPath = *path;
        m_status.segmentCount = 0;
        EmitStatus();
        return;
    }

    bool anchored = sc.anchored;
    std::string replayPathStr = sc.replayPath;

    if (anchored) {
        // Fast path: the post-match linker already shifted each segment onto
        // the replay-frame timeline and stored the .replay path. Skip
        // ReplayLocator + binary header parse on the open path.
        m_frameTime = sc.frameTime;
        std::cerr << "[PlaybackController] sidecar pre-anchored (linked at save time): "
                  << segs.size() << " segments, replay=" << replayPathStr
                  << " frameTime=" << m_frameTime << "\n";
    } else {
        // Live anchoring: post-match linker hadn't run (e.g., older sidecar
        // from before this feature, or .replay never materialized). Same
        // per-goal model as StartPostMatchLink: one offset per segment from
        // its GoalScored stamp (or segment end for legacy sidecars) vs the
        // header's goalFrames.
        //
        // Locate the .replay by time (RL headers have no MatchGuid). New
        // sidecars store matchStartEpoch; legacy ones don't, so fall back to
        // the sidecar file's mtime (~match end) with a wider tolerance —
        // FindByEpoch matches on either the start OR the end timestamp.
        int64_t targetEpoch = sc.matchStartEpoch;
        double tolerance = 180.0;
        if (targetEpoch <= 0) {
            std::error_code ec;
            auto ftime = std::filesystem::last_write_time(*path, ec);
            if (!ec) {
                // C++17 has no std::filesystem -> system_clock conversion
                // (that's C++20's file_clock::to_sys), so bridge the two
                // clocks by their current "now" offset.
                const auto sysNow = std::chrono::system_clock::now();
                const auto fileNow = std::filesystem::file_time_type::clock::now();
                const auto sysTp = std::chrono::time_point_cast<
                    std::chrono::system_clock::duration>(
                        ftime - fileNow + sysNow);
                targetEpoch = std::chrono::duration_cast<std::chrono::seconds>(
                    sysTp.time_since_epoch()).count();
                tolerance = 1800.0;  // mtime is a looser proxy for match time
            }
        }
        auto replayPath = m_replayLocator->FindByEpoch(targetEpoch, tolerance);
        if (!replayPath && !matchGuid.empty()) {
            replayPath = m_replayLocator->FindByMatchGuid(matchGuid);
        }
        if (replayPath) {
            replayPathStr = replayPath->u8string();
            ReplayMetadata meta;
            if (ParseReplayHeader(*replayPath, meta) && !meta.goalFrames.empty()) {
                const double frameTime = meta.FrameTime();
                AnchorPlan plan =
                    ComputeAnchorPlan(segs, meta.goalFrames, frameTime);
                if (plan.mode != AnchorMode::None) {
                    ApplyAnchorPlan(segs, plan);
                    m_frameTime = frameTime;
                    anchored = true;

                    std::cerr << "[PlaybackController] live-anchored [Anchor] "
                              << DescribeAnchorPlan(plan) << "\n";
                    for (size_t i = 0; i < segs.size(); ++i) {
                        const double startFrame = segs[i].startTimeSec / frameTime;
                        const double endFrame   = segs[i].endTimeSec   / frameTime;
                        std::cerr << "  seg[" << i << "] frames ["
                                  << static_cast<long>(startFrame) << ".."
                                  << static_cast<long>(endFrame) << "] = sec ["
                                  << segs[i].startTimeSec << ".."
                                  << segs[i].endTimeSec << "]  duration="
                                  << (segs[i].endTimeSec - segs[i].startTimeSec)
                                  << "s offset=" << plan.offsets[i] << "\n";
                    }
                } else {
                    std::cerr << "[PlaybackController] anchor failed: "
                              << plan.note
                              << " (numFrames=" << meta.numFrames
                              << " goals=" << meta.goalFrames.size()
                              << " segments=" << segs.size()
                              << ") — playing with captured stamps as-is\n";
                }
            } else {
                std::cerr << "[PlaybackController] anchor failed: "
                          << "numFrames=" << meta.numFrames
                          << " goals=" << meta.goalFrames.size()
                          << " segments=" << segs.size()
                          << " — playing with captured stamps as-is\n";
            }
        } else {
            std::cerr << "[PlaybackController] no .replay matched MatchGuid "
                      << matchGuid << " — playing with captured stamps as-is\n";
        }
    }

    m_anchored = anchored;
    m_playback->LoadSegments(segs);
    m_syncState->isInReplay.store(true);
    m_status.phase = PlaybackPhase::Playing;
    m_status.sidecarPath = *path;
    m_status.replayPath = replayPathStr;
    m_status.segmentCount = static_cast<int>(segs.size());
    m_status.anchored = anchored;
    EmitStatus();
}

void PlaybackController::ExitReplay() {
    if (!m_inReplay) return;
    m_inReplay = false;
    m_syncState->isInReplay.store(false);
    if (m_playback) m_playback->StopPlayback();
    m_status = PlaybackStatus{};
    m_anchored = false;
    m_frameTime = 1.0 / 30.0;
    m_elapsedFloor = 0.0;
    m_haveElapsedFloor = false;
    m_lastLoggedFrame = -1;
    m_lastLoggedSegIdx = -2;
    EmitStatus();
}

void PlaybackController::HandleEvent(const std::string& eventName,
                                     const std::string& dataJson) {
    nlohmann::json data;
    try {
        data = dataJson.empty() ? nlohmann::json::object()
                                : nlohmann::json::parse(dataJson);
    } catch (...) {
        return;
    }
    if (!data.is_object()) return;

    // Only UpdateState ticks carry the per-frame replay state we care about.
    if (eventName != "UpdateState" && eventName != "GameState") {
        // Lifecycle events like MatchDestroyed should clean up if the user
        // closes the replay back to the menu without bReplay flipping false.
        if (eventName == "MatchDestroyed") {
            std::lock_guard<std::mutex> lock(m_mutex);
            ExitReplay();
        }
        return;
    }

    auto game = data.contains("Game") && data["Game"].is_object()
                   ? data["Game"]
                   : (data.contains("game") && data["game"].is_object()
                         ? data["game"]
                         : nlohmann::json::object());

    int frame = 0;
    bool hasFrame = false;
    if (game.contains("Frame") && game["Frame"].is_number()) {
        frame = game["Frame"].get<int>();
        hasFrame = true;
    } else if (data.contains("Frame") && data["Frame"].is_number()) {
        frame = data["Frame"].get<int>();
        hasFrame = true;
    }

    bool bReplayRaw = false;
    if (game.contains("bReplay") && game["bReplay"].is_boolean())
        bReplayRaw = game["bReplay"].get<bool>();
    else if (data.contains("bReplay") && data["bReplay"].is_boolean())
        bReplayRaw = data["bReplay"].get<bool>();

    // bReplay=true with NO Frame is an in-match goal cinematic, not a saved
    // replay viewer. Without this gate the controller would EnterReplay on
    // every cinematic and the UI would flash a "Replay opened" banner during
    // every goal celebration.
    bool isReplay = bReplayRaw && hasFrame;

    // Sync time base: Frame, NOT Elapsed.
    //
    // `Elapsed` is the in-game clock — it freezes during goal cinematics and
    // kickoff countdowns. Using it for audio sync makes the audio fall
    // progressively behind by the cumulative dead-time wall-clock duration
    // (one cinematic + one kickoff ≈ 5–10s of lag per goal).
    //
    // `Frame` is the replay file's recording counter: monotonic, never
    // freezes, and shares its coordinate system with `goalFrames[i]` from
    // the .replay header (which anchoring maps segments onto). So
    // Frame * frameTime is the right thing to feed into AudioPlaybackManager.
    //
    // Read Elapsed only for the diagnostic log line; not used for sync.
    double elapsed = 0.0;
    if (game.contains("Elapsed") && game["Elapsed"].is_number())
        elapsed = game["Elapsed"].get<double>();
    else if (data.contains("Elapsed") && data["Elapsed"].is_number())
        elapsed = data["Elapsed"].get<double>();

    std::string matchGuid;
    if (data.contains("MatchGuid") && data["MatchGuid"].is_string())
        matchGuid = data["MatchGuid"].get<std::string>();

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!isReplay) {
        if (m_inReplay) ExitReplay();
        return;
    }

    // We're in replay mode now.
    if (!m_inReplay) {
        m_inReplay = true;
        EnterReplay(matchGuid);
    } else if (!matchGuid.empty() && matchGuid != m_status.matchGuid) {
        // Switched to a different replay without a MatchDestroyed in between.
        m_playback->StopPlayback();
        EnterReplay(matchGuid);
    }

    // Computed after EnterReplay so m_frameTime reflects this replay's
    // RecordFPS rather than the previous session's value.
    double frameSec = static_cast<double>(frame) * m_frameTime;

    // Anchored path: segment.startTimeSec / endTimeSec are in replay-frame
    // seconds (rewritten in EnterReplay from goalFrames[i] * frameTime).
    // Frame from UpdateState is in the same coordinate system, so feed
    // frameSec directly — no calibration.
    //
    // Fallback path: if anchoring failed (no .replay, parse error, segment
    // count mismatch), capture's stamps are wall-clock-since-first-live-
    // UpdateState. The replay's Frame=0 corresponds to wall-clock-since-
    // replay-recording-start, which is BEFORE the live match started by
    // the replay's leader/preroll. We can't recover that offset without
    // more metadata, so we calibrate against the first observed Frame and
    // hope the user opened the replay at the start. (Capture ONCE; never
    // updated — a backward scrub mustn't re-baseline.)
    double effectiveSec;
    if (m_anchored) {
        effectiveSec = frameSec;
    } else {
        if (!m_haveElapsedFloor) {
            m_elapsedFloor = frameSec;
            m_haveElapsedFloor = true;
        }
        effectiveSec = frameSec - m_elapsedFloor;
    }

    m_syncState->currentReplayFrame.store(frame);
    int activeSeg = -1;
    if (m_status.phase == PlaybackPhase::Playing && m_playback) {
        m_playback->SyncToReplayTime(static_cast<float>(effectiveSec));
        activeSeg = m_syncState->activeSegmentIndex.load();
    }

    bool segChanged = (activeSeg != m_status.activeSegmentIndex);
    bool elapsedTicked = std::abs(effectiveSec - m_status.elapsedSec) > 0.05;
    if (segChanged || elapsedTicked) {
        m_status.elapsedSec = effectiveSec;
        m_status.activeSegmentIndex = activeSeg;
        EmitStatus();
    }

    // Throttled diagnostic: one line per ~1s of replay frames, plus one
    // immediately whenever the active segment changes. The user reads Frame
    // here at the visual goal moment to compare against .replay's goalFrames.
    bool logSegChanged = (activeSeg != m_lastLoggedSegIdx);
    bool frameTick  = (m_lastLoggedFrame < 0 ||
                       std::abs(frame - m_lastLoggedFrame) >= 30);
    if (logSegChanged || frameTick) {
        std::cerr << "[playback] tick frame=" << frame
                  << " frameSec=" << frameSec
                  << " elapsed=" << elapsed
                  << " effSec=" << effectiveSec
                  << " seg=" << activeSeg
                  << " bReplay=" << (isReplay ? 1 : 0)
                  << "\n";
        m_lastLoggedFrame = frame;
        m_lastLoggedSegIdx = activeSeg;
    }
}

} // namespace SyncComms
