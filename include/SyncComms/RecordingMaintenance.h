#pragma once

#include <functional>
#include <string>

namespace SyncComms {

class ReplayLocator;

using MaintenanceLogFn = std::function<void(const std::string&)>;

/// Number of days after which an *unbound* recording (no matching .replay on
/// disk) is eligible for automatic deletion. Bound recordings are kept forever.
constexpr double kRetentionDays = 14.0;

/// Pure decision predicate (no I/O — unit-testable): a recording should be
/// pruned only when it is NOT bound to an existing replay AND it is strictly
/// older than `maxAgeDays`. Anything bound, or younger than the window, stays.
inline bool ShouldPruneUnbound(bool boundToReplay, double ageDays,
                               double maxAgeDays = kRetentionDays) {
    return !boundToReplay && ageDays > maxAgeDays;
}

/// Delete every `.wav` in `outputDir` that is NOT the sole on-disk copy of a
/// sidecar segment. This removes two kinds of leftover:
///   - redundant WAVs whose audio was already compressed into a sidecar's
///     embedded `audioData` (normal-flow files that escaped the post-compress
///     cleanup when one segment failed), and
///   - true orphans from a capture that crashed/was killed before its sidecar
///     was written (no sidecar references them at all).
/// A WAV that a sidecar references WITHOUT embedded `audioData` is the only
/// copy of that audio, so it is preserved. Intended to run ONCE at startup,
/// before any capture begins (no race with an in-flight recording).
/// Returns the number of files deleted.
int SweepOrphanWavs(const std::string& outputDir, const MaintenanceLogFn& log);

/// Delete sidecars older than `maxAgeDays` whose match has no corresponding
/// `.replay` on disk (the audio can never be synced to anything, so it's dead
/// weight). Boundness is decided by the sidecar's recorded `replayPath` (if it
/// still exists) or by matching its `matchStartEpoch` against `locator`.
/// Skipped entirely (returns 0) when the Rocket League demos directory is
/// unavailable, so a moved/uninstalled RL folder can never wipe the library.
/// Returns the number of sidecars deleted.
int PruneUnboundSidecars(const std::string& outputDir, ReplayLocator& locator,
                         double maxAgeDays, const MaintenanceLogFn& log);

/// Delete a single recording by its replay id / match guid: the sidecar JSON
/// plus any WAV files it still references. Rejects ids containing path
/// separators or "..". Returns true if a sidecar was found and removed.
bool DeleteRecording(const std::string& outputDir, const std::string& matchGuid,
                     const MaintenanceLogFn& log);

} // namespace SyncComms
