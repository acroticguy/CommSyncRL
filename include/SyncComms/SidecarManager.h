#pragma once

#include "SyncComms/SyncState.h"
#include "SyncComms/Config.h"
#include <functional>
#include <string>
#include <vector>
#include <optional>

namespace SyncComms {

/// Top-level metadata + segment list, as parsed from a sidecar JSON.
struct SidecarReadResult {
    std::vector<SegmentInfo> segments;
    /// True if a previous post-match linker has already anchored the segment
    /// times to a .replay file's goalFrames. When set, PlaybackController
    /// can skip its own anchoring work on EnterReplay.
    bool anchored = false;
    /// Absolute path to the .replay file that was used to anchor, or empty.
    std::string replayPath;
    /// Seconds per replay frame as recorded in the sidecar (typically 1/30).
    double frameTime = 1.0 / 30.0;
    /// Unix seconds at match start (first live UpdateState). Used to match the
    /// sidecar to its .replay by time, since RL replay headers carry no
    /// MatchGuid. 0 if absent (legacy sidecars written before this field).
    int64_t matchStartEpoch = 0;
};

class SidecarManager {
public:
    using LogFn = std::function<void(const std::string&)>;

    /// `log` is invoked for non-fatal warnings (parse failures, version mismatches,
    /// post-compress reports). Pass an empty std::function to silence.
    SidecarManager(Config* config, LogFn log = {});

    /// Write sidecar JSON for a completed capture session. Optionally records
    /// that the segment times have already been anchored to `replayPath`'s
    /// goalFrames so PlaybackController can skip live anchoring later.
    /// Atomic on the same volume (writes to .tmp, then renames into place).
    bool WriteSidecar(const std::string& replayId,
                      const std::vector<SegmentInfo>& segments,
                      bool anchored = false,
                      const std::string& replayPath = {},
                      const std::string& anchorMode = {},
                      int64_t matchStartEpoch = 0);

    /// Read segments + top-level metadata from an existing sidecar JSON.
    SidecarReadResult ReadSidecar(const std::string& filePath);

    /// Search for a sidecar file matching a replay ID.
    std::optional<std::string> FindSidecar(const std::string& replayId);

    /// Search for a sidecar whose segments contain the given frame.
    std::optional<std::string> FindSidecarByFrame(int replayFrame);

    /// Compress WAV segments to OGG Vorbis files. Updates audioFile references and
    /// rewrites the sidecar. Deletes WAV files on success.
    void CompressSegments(const std::string& replayId, std::vector<SegmentInfo>& segments);

private:
    std::string BuildSidecarPath(const std::string& replayId);
    Config* m_config;
    LogFn   m_log;
};

} // namespace SyncComms
