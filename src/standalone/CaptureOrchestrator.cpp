#include "SyncComms/CaptureOrchestrator.h"
#include "SyncComms/ReplayLocator.h"
#include "SyncComms/ReplayMetadata.h"
#include "SyncComms/SegmentAnchoring.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

namespace SyncComms {

namespace {

double WallSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

const char* PhaseStr(CapturePhase p) {
    switch (p) {
        case CapturePhase::Idle:       return "idle";
        case CapturePhase::Armed:      return "armed";
        case CapturePhase::Recording:  return "recording";
        case CapturePhase::Paused:     return "paused";
        case CapturePhase::Finalizing: return "finalizing";
        case CapturePhase::Error:      return "error";
    }
    return "idle";
}

} // namespace

CaptureOrchestrator::CaptureOrchestrator(StandaloneConfig& config)
    : m_config(config)
    , m_syncState(std::make_shared<SyncState>())
{
    auto log = [](const std::string& msg) {
        std::cerr << "[CaptureOrchestrator] " << msg << std::endl;
    };
    m_capture = std::make_unique<AudioCaptureManager>(m_syncState, &m_config);
    m_sidecar = std::make_unique<SidecarManager>(&m_config, log);
}

CaptureOrchestrator::~CaptureOrchestrator() {
    Shutdown();
}

void CaptureOrchestrator::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_capture && m_capture->IsCapturing()) {
        m_capture->StopCapture(m_lastFrame);
    }
    m_status.phase = CapturePhase::Idle;
    EmitStatus();
}

CaptureStatus CaptureOrchestrator::GetStatus() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void CaptureOrchestrator::EmitStatus() {
    // Caller must hold m_mutex while computing status; we copy then drop the
    // lock before invoking the callback so callbacks can call back in safely.
    CaptureStatus snap = m_status;
    StatusCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_cbMutex);
        cb = m_onStatus;
    }
    if (cb) cb(snap);
}

void CaptureOrchestrator::Reset() {
    m_segments.clear();
    m_currentMatchGuid.clear();
    m_currentSegmentIndex = 0;
    m_lastFrame = 0;
    m_matchStartWall = 0.0;
    m_matchStartSystemEpoch = 0;
    m_lastElapsed = 0.0;
    m_segmentStartElapsed = 0.0;
    m_segmentStartWall = 0.0;
    m_pendingGoalElapsed = -1.0;
    m_lastBReplay = -1;
    m_lastRawBReplay = -1;
    m_cinematicJustStarted = false;
    m_pendingSegmentStart = false;
    m_status.matchGuid.clear();
    m_status.segmentCount = 0;
    m_status.segmentSeconds = 0.0;
    m_status.lastError.clear();
}

void CaptureOrchestrator::StartNewMatch(const std::string& matchGuid) {
    if (!m_config.IsEnabled()) {
        m_status.phase = CapturePhase::Idle;
        EmitStatus();
        return;
    }

    // If we somehow re-arm during a live match (rare), discard pending state.
    if (m_capture && m_capture->IsCapturing()) {
        m_capture->StopCapture(m_lastFrame);
    }
    Reset();

    m_currentMatchGuid = matchGuid;
    m_status.matchGuid = matchGuid;
    // Stay Armed until the first UpdateState confirms bReplay=false. The Stats
    // API re-fires MatchCreated/CountdownBegin while opening a saved replay,
    // so we can't trust those alone to mean "the user is playing live."
    m_status.phase = CapturePhase::Armed;
    EmitStatus();
}

void CaptureOrchestrator::StartNewSegment() {
    if (!m_capture) return;
    try {
        m_capture->StartCapture(m_currentMatchGuid, m_currentSegmentIndex, m_lastFrame);
        m_segmentStartWall = WallSeconds();
        m_segmentStartElapsed = m_lastElapsed;
        m_pendingGoalElapsed = -1.0;
        m_status.phase = CapturePhase::Recording;
    } catch (const std::exception& e) {
        m_status.phase = CapturePhase::Error;
        m_status.lastError = std::string("StartCapture failed: ") + e.what();
    }
    EmitStatus();
}

void CaptureOrchestrator::CloseCurrentSegment(int endFrame) {
    if (!m_capture || !m_capture->IsCapturing()) return;
    try {
        SegmentInfo seg = m_capture->StopCapture(endFrame);
        // AudioCaptureManager doesn't fill these — the BakkesMod plugin used
        // to stamp them after the fact. The playback path requires both > 0
        // to even consider a segment for FindSegmentForTime, so missing them
        // means audio never plays back. We use the replay/match `Elapsed`
        // observed via UpdateState as the time base on both sides.
        seg.startTimeSec = m_segmentStartElapsed;
        seg.endTimeSec   = m_lastElapsed;
        seg.goalTimeSec  = m_pendingGoalElapsed;
        m_pendingGoalElapsed = -1.0;
        std::cerr << "[CaptureOrchestrator] segment " << seg.index
                  << " closed: start=" << seg.startTimeSec
                  << " end=" << seg.endTimeSec
                  << " goal=" << seg.goalTimeSec << "\n";
        m_segments.push_back(std::move(seg));
        m_status.segmentCount = static_cast<int>(m_segments.size());
    } catch (const std::exception& e) {
        m_status.phase = CapturePhase::Error;
        m_status.lastError = std::string("StopCapture failed: ") + e.what();
        EmitStatus();
    }
}

void CaptureOrchestrator::Finalize() {
    if (m_status.phase == CapturePhase::Idle) return;

    CloseCurrentSegment(m_lastFrame);

    // Write whatever we have. Both MatchEnded (clean finish) and
    // MatchDestroyed-without-MatchEnded (alt-F4, network drop, exit-to-menu,
    // forfeit timeout) take this path — we always keep the audio. The user's
    // explicit policy: we don't ask, we don't lose recordings.
    //
    // We only skip when there's literally nothing captured: phase was Armed
    // without ever transitioning to Recording (e.g. the user opened a replay
    // and we correctly disarmed before any segments existed).
    if (m_segments.empty() || m_currentMatchGuid.empty()) {
        Reset();
        m_status.phase = CapturePhase::Idle;
        EmitStatus();
        return;
    }

    m_status.phase = CapturePhase::Finalizing;
    EmitStatus();

    try {
        m_sidecar->CompressSegments(m_currentMatchGuid, m_segments);
        m_sidecar->WriteSidecar(m_currentMatchGuid, m_segments,
                                /*anchored=*/false, /*replayPath=*/{},
                                /*anchorMode=*/{}, m_matchStartSystemEpoch);
    } catch (const std::exception& e) {
        m_status.phase = CapturePhase::Error;
        m_status.lastError = std::string("Finalize failed: ") + e.what();
        EmitStatus();
        Reset();
        m_status.phase = CapturePhase::Idle;
        EmitStatus();
        return;
    }

    // Snapshot before Reset() clears the orchestrator's state, then hand off
    // to a background worker that waits for RL to write the .replay file and
    // pre-anchors the sidecar to its goalFrames. This eliminates the
    // ReplayLocator scan + binary header parse that would otherwise stutter
    // when the user opens the replay viewer minutes/hours later.
    std::string matchGuidCopy = m_currentMatchGuid;
    std::vector<SegmentInfo> segmentsCopy = m_segments;
    int64_t epochCopy = m_matchStartSystemEpoch;

    Reset();
    m_status.phase = CapturePhase::Idle;
    EmitStatus();

    StartPostMatchLink(std::move(matchGuidCopy), std::move(segmentsCopy), epochCopy);
}

void CaptureOrchestrator::StartPostMatchLink(std::string matchGuid,
                                              std::vector<SegmentInfo> segments,
                                              int64_t matchStartEpoch) {
    if (matchGuid.empty() || segments.empty()) return;

    StandaloneConfig* configPtr = &m_config;
    std::thread([configPtr, matchStartEpoch,
                 matchGuid = std::move(matchGuid),
                 segments  = std::move(segments)]() mutable {
        // RL writes the .replay shortly after MatchEnded, but the exact
        // delay varies (autosave vs save-on-exit, disk speed, etc.). Poll
        // for up to ~120 s before giving up. The sidecar already exists
        // and is playable; the linker is a pure optimization.
        //
        // Match by time, not MatchGuid: RL replay headers carry no MatchGuid,
        // so we find the .replay whose header MatchStartEpoch is closest to
        // this match's start epoch.
        ReplayLocator locator;
        std::filesystem::path replayPath;
        for (int attempt = 0; attempt < 60 && replayPath.empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            auto found = locator.FindByEpoch(matchStartEpoch);
            if (found) replayPath = *found;
        }

        if (replayPath.empty()) {
            std::cerr << "[CaptureOrchestrator] post-match link: no .replay "
                         "near epoch " << matchStartEpoch
                      << " after 120s — sidecar will be live-anchored on open\n";
            return;
        }

        ReplayMetadata meta;
        if (!ParseReplayHeader(replayPath, meta) || meta.goalFrames.empty()) {
            std::cerr << "[CaptureOrchestrator] post-match link: header parse "
                         "incompatible (numFrames=" << meta.numFrames
                      << " goals=" << meta.goalFrames.size()
                      << " segments=" << segments.size() << ")\n";
            return;
        }

        // Per-goal anchoring: the capture wall-clock and the replay's frame
        // timeline diverge between rounds (cinematics/dead time the replay
        // doesn't record), so each segment gets its own offset derived from
        // its GoalScored stamp vs the header's goalFrames. Legacy sidecars
        // without stamps fall back to anchoring each segment's end.
        const double frameTime = meta.FrameTime();
        AnchorPlan plan = ComputeAnchorPlan(segments, meta.goalFrames, frameTime);
        if (plan.mode == AnchorMode::None) {
            std::cerr << "[PostMatchLink] " << matchGuid << " not anchored: "
                      << plan.note << "\n";
            return;
        }
        ApplyAnchorPlan(segments, plan);
        for (auto& seg : segments) seg.frameTime = frameTime;

        // Use a fresh SidecarManager — the orchestrator's m_sidecar may be
        // gone by now if the app shut down, and the manager is cheap.
        SidecarManager mgr(configPtr);
        if (mgr.WriteSidecar(matchGuid, segments, /*anchored=*/true,
                             replayPath.u8string(), AnchorModeStr(plan.mode),
                             matchStartEpoch))
        {
            std::cerr << "[PostMatchLink] " << matchGuid
                      << " -> " << replayPath.u8string()
                      << " [Anchor] " << DescribeAnchorPlan(plan) << "\n";
        }
    }).detach();
}

void CaptureOrchestrator::HandleEvent(const std::string& eventName,
                                      const std::string& dataJson) {
    nlohmann::json data;
    try {
        data = dataJson.empty() ? nlohmann::json::object()
                                : nlohmann::json::parse(dataJson);
    } catch (...) {
        data = nlohmann::json::object();
    }

    auto guidOf = [&]() -> std::string {
        if (data.contains("MatchGuid") && data["MatchGuid"].is_string())
            return data["MatchGuid"].get<std::string>();
        if (data.contains("matchGuid") && data["matchGuid"].is_string())
            return data["matchGuid"].get<std::string>();
        return {};
    };

    std::lock_guard<std::mutex> lock(m_mutex);

    // Track bReplay from Game (only field that's reliably present in every
    // UpdateState during a live match). Frame and Elapsed are NOT present
    // during a live match — they only exist when bReplay=true (i.e., the
    // user is watching a saved replay) — so we can't use them as the time
    // base for capture. Instead we derive elapsed from wall-clock relative
    // to the first UpdateState of the match.
    if (data.contains("Game") && data["Game"].is_object()) {
        auto& g = data["Game"];
        bool hasFrame = g.contains("Frame") && g["Frame"].is_number();
        if (hasFrame) m_lastFrame = g["Frame"].get<int>();
        if (g.contains("bReplay") && g["bReplay"].is_boolean()) {
            bool br = g["bReplay"].get<bool>();
            // bReplay=true with NO Frame = in-match goal cinematic — not a
            // saved-replay viewer. Empirically (and per the doc) Frame and
            // Elapsed are populated only during saved-replay playback;
            // they're null both during live play AND during in-match
            // cinematics. So we only treat bReplay=true as "saved replay"
            // when Frame is also present.
            m_lastBReplay = (br && hasFrame) ? 1 : 0;

            // The raw 0->1 flip marks the cinematic beginning ~3.5s after
            // the goal (measured from recorded streams). This is our
            // segment-close boundary: the documented GoalReplayStart never
            // appears on the wire on current RL builds.
            int raw = br ? 1 : 0;
            if (m_lastRawBReplay == 0 && raw == 1) {
                m_cinematicJustStarted = true;
            }
            m_lastRawBReplay = raw;
        }
    }

    if (eventName == "UpdateState" || eventName == "GameState") {
        if (m_matchStartWall == 0.0) {
            m_matchStartWall = WallSeconds();
            // System-clock epoch of match start, to match the .replay header's
            // MatchStartEpoch later (steady_clock can't be turned into a
            // wall-clock epoch).
            m_matchStartSystemEpoch =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
        }
        m_lastElapsed = WallSeconds() - m_matchStartWall;
    }

    if (eventName == "UpdateState" || eventName == "GameState") {
        m_lastUpdateStateRaw = dataJson;

        // Cinematic just began (raw bReplay flipped 0->1): close the round's
        // segment here, the same boundary GoalReplayStart was meant to give
        // us. m_lastElapsed was refreshed above, so the segment end stamp is
        // this tick. The goal stamp (m_pendingGoalElapsed) was set ~3.5s ago
        // by the live GoalScored; celebration audio in between is kept.
        if (m_cinematicJustStarted) {
            m_cinematicJustStarted = false;
            if (m_status.phase == CapturePhase::Recording ||
                m_status.phase == CapturePhase::Paused) {
                std::cerr << "[CaptureOrchestrator] cinematic started "
                             "(bReplay flip) — closing segment "
                          << m_currentSegmentIndex << "\n";
                CloseCurrentSegment(m_lastFrame);
                ++m_currentSegmentIndex;
                m_status.phase = CapturePhase::Armed;
                EmitStatus();
            }
        }

        // We do NOT auto-start a segment here. Segments start on
        // CountdownBegin (start of each round's 3-2-1 countdown), matching
        // the BakkesMod plugin's hook on Countdown.BeginState. If a
        // CountdownBegin already arrived before this UpdateState, the
        // pending flag fires the deferred start now.
        //
        // m_lastBReplay==1 here means we saw a saved-replay UpdateState
        // (Frame + bReplay=true) — disarm. In-match cinematics never set
        // m_lastBReplay to 1 because their Frame is null (see above).
        if (m_status.phase == CapturePhase::Armed && m_lastBReplay != -1) {
            if (m_lastBReplay == 1) {
                Reset();
                m_status.phase = CapturePhase::Idle;
                EmitStatus();
            } else if (m_pendingSegmentStart) {
                m_pendingSegmentStart = false;
                StartNewSegment();
            }
        }

        if (m_status.phase == CapturePhase::Recording) {
            m_status.segmentSeconds = WallSeconds() - m_segmentStartWall;
            EmitStatus();
        }
    }

    if (eventName == "MatchCreated") {
        std::string guid = guidOf();
        if (!guid.empty() && guid != m_currentMatchGuid) {
            StartNewMatch(guid);
        }
        return;
    }

    // CountdownBegin = start of a round's 3-2-1 countdown. Fires at first
    // kickoff of the match AND after every goal. This is the BakkesMod-style
    // segment-start trigger (equivalent to Countdown.BeginState). Each
    // segment captures from countdown through ball-in-net; the cinematic
    // and post-cinematic dead time are intentionally NOT captured, so the
    // saved replay's audio falls silent between rounds.
    if (eventName == "CountdownBegin") {
        std::string guid = guidOf();
        if (!guid.empty() && guid != m_currentMatchGuid) {
            StartNewMatch(guid);
        }
        if (m_status.phase == CapturePhase::Armed) {
            if (m_lastBReplay == 0) {
                StartNewSegment();
            } else if (m_lastBReplay == -1) {
                // bReplay not yet observed — defer until next UpdateState
                // confirms this is a live match.
                m_pendingSegmentStart = true;
            }
            // If m_lastBReplay==1 we're in a saved-replay viewer; ignore.
        }
        return;
    }

    // GoalReplayStart is documented as the cinematic-begin event, but
    // recorded wire data (tools/stats_api_raw_*.jsonl, 2026-04-30) shows it
    // is NOT emitted on current RL builds — the raw bReplay 0->1 flip in
    // UpdateState (handled above) is the real boundary. Kept as a fallback
    // for builds/modes that do emit it; if the flip already closed the
    // segment, the phase is Armed and this no-ops.
    if (eventName == "GoalReplayStart") {
        std::cerr << "[CaptureOrchestrator] GoalReplayStart phase="
                  << PhaseStr(m_status.phase)
                  << " bReplay=" << m_lastBReplay
                  << " frame=" << m_lastFrame << "\n";
        if (m_status.phase == CapturePhase::Recording) {
            CloseCurrentSegment(m_lastFrame);
            ++m_currentSegmentIndex;
            m_status.phase = CapturePhase::Armed;
            EmitStatus();
        }
        return;
    }

    // GoalScored fires on the exact tick the ball crosses the line —
    // event-driven, independent of PacketSendRate. It is NOT the segment
    // boundary (GoalReplayStart above closes the segment a few seconds later,
    // keeping the celebration audio), but it IS the anchor point: the .replay
    // header's goalFrames[i] marks this same moment on the replay timeline,
    // so stamping it lets the anchorer compute an exact per-segment offset.
    if (eventName == "GoalScored") {
        // Gates, all validated against recorded streams:
        //  - raw bReplay must be 0: GoalScored RE-FIRES during the in-match
        //    cinematic (~5-7s late, GoalTime=0, empty Scorer) and must not
        //    take the stamp;
        //  - first-wins: if a cinematic flip was somehow missed and two real
        //    goals land in one segment, anchor on the first (the anchorer's
        //    count-mismatch handling covers the rest).
        double stamp = -1.0;
        if ((m_status.phase == CapturePhase::Recording ||
             m_status.phase == CapturePhase::Paused) &&
            m_matchStartWall > 0.0 &&
            m_lastRawBReplay == 0 &&
            m_pendingGoalElapsed < 0.0)
        {
            // Fresh wall read, not m_lastElapsed — that's up to one
            // UpdateState tick (~33ms at 30Hz) stale.
            stamp = WallSeconds() - m_matchStartWall;
            m_pendingGoalElapsed = stamp;
        }
        double apiGoalTime = -1.0;
        if (data.contains("GoalTime") && data["GoalTime"].is_number())
            apiGoalTime = data["GoalTime"].get<double>();
        std::cerr << "[CaptureOrchestrator] GoalScored phase="
                  << PhaseStr(m_status.phase)
                  << " bReplay=" << m_lastBReplay
                  << " frame=" << m_lastFrame
                  << " stamp=" << stamp
                  << "s (segStart=" << m_segmentStartElapsed
                  << " roundLen=" << (stamp - m_segmentStartElapsed)
                  << " apiGoalTime=" << apiGoalTime << ")\n";
        return;
    }
    // Ignore StatfeedEvent — we used to listen for Type=Goal here as a
    // segment trigger, but GoalReplayStart above covers that more reliably.
    if (eventName == "StatfeedEvent") {
        return;
    }

    if (eventName == "MatchPaused") {
        if (m_status.phase == CapturePhase::Recording) {
            // We don't actually stop the WASAPI device — the existing
            // AudioCaptureManager doesn't have an explicit pause API. We just
            // note the phase so the UI reflects pause; audio continues to
            // accumulate, which is OK for short pause windows in RL.
            m_status.phase = CapturePhase::Paused;
            EmitStatus();
        }
        return;
    }
    if (eventName == "MatchUnpaused") {
        if (m_status.phase == CapturePhase::Paused) {
            m_status.phase = CapturePhase::Recording;
            EmitStatus();
        }
        return;
    }

    if (eventName == "MatchEnded") {
        Finalize();
        return;
    }
    if (eventName == "MatchDestroyed") {
        // If MatchEnded already fired we're Idle here and Finalize is a no-op.
        // Otherwise this is a forfeit-without-end / alt-F4 / disconnect — we
        // still write the sidecar so the audio is preserved.
        if (m_status.phase != CapturePhase::Idle &&
            m_status.phase != CapturePhase::Finalizing) {
            Finalize();
        }
        return;
    }
}

} // namespace SyncComms
