#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace SyncComms {

/// The few values we extract from a Rocket League .replay file's property
/// header. Used by PlaybackController to anchor captured audio segments to the
/// canonical replay timeline rather than calibrating against the first
/// `Elapsed` we see (which is fragile under scrubbing).
struct ReplayMetadata {
    // NOTE (2026-04-30, RL Season 22): current RL replay headers do NOT
    // contain MatchGuid, NumFrames, or RecordFPS — verified by raw-byte scan
    // across 116 real files (tools/check_guid_join.py). What IS present and
    // early in the header: Goals[].frame, MatchStartEpoch, TotalSecondsPlayed.
    // So matching a sidecar to a .replay is done by MatchStartEpoch (time),
    // NOT by MatchGuid, and anchoring keys on goalFrames alone.
    std::string matchGuid;             // Property "MatchGuid" — usually ABSENT now
    int numFrames = 0;                 // Property "NumFrames" — usually ABSENT now
    double recordFps = 30.0;           // Property "RecordFPS" — usually ABSENT now (-> 30fps)
    int64_t matchStartEpoch = 0;       // Property "MatchStartEpoch" — Unix seconds at match start
    double totalSecondsPlayed = 0.0;   // Property "TotalSecondsPlayed" — match wall length
    std::vector<int> goalFrames;       // Property "Goals" -> each element's "frame"

    /// Seconds per replay frame, guarded against a missing/absurd RecordFPS.
    double FrameTime() const {
        return (recordFps > 1.0) ? (1.0 / recordFps) : (1.0 / 30.0);
    }
};

/// Parse just the property header of a .replay file. Returns true if anything
/// usable for anchoring was extracted (goalFrames, a frame count, or a match
/// start epoch). The individual fields are best-effort and may be absent.
bool ParseReplayHeader(const std::filesystem::path& path, ReplayMetadata& out);

} // namespace SyncComms
