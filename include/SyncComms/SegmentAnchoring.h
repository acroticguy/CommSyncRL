#pragma once

// Maps capture-clock segment timestamps onto the replay-frame timeline.
//
// Capture stamps segments in wall-clock seconds since the first UpdateState
// of the match; the replay viewer reports time as Frame * frameTime. The two
// clocks advance ~1:1 while a round is live, but diverge between rounds:
// wall-clock keeps running through goal cinematics and dead time that the
// .replay file does not record (and the user can skip). So no single offset
// can align a whole match — each segment needs its own.
//
// Anchor points: GoalScored is stamped at the exact goal tick on the capture
// clock (SegmentInfo::goalTimeSec); the .replay header lists the same moment
// as goalFrames[i] on the replay timeline. Pairing them in order yields one
// exact offset per goal-terminated segment.
//
// Pure functions, no I/O — compiled into the test target.

#include "SyncComms/SyncState.h"

#include <string>
#include <vector>

namespace SyncComms {

enum class AnchorMode {
    PerGoal,       // exact: goalTimeSec stamps matched to goalFrames
    LegacyEnd,     // pre-goal-stamp sidecars: endTimeSec matched to goalFrames
                   // (constant celebration-length early shift per segment)
    SingleOffset,  // degraded: one match-wide offset (ambiguous/inconsistent data)
    None,          // nothing to anchor against (no goals / no segments)
};

const char* AnchorModeStr(AnchorMode mode);

struct AnchorPlan {
    AnchorMode mode = AnchorMode::None;
    /// One offset per segment (replay seconds − capture seconds). Empty when
    /// mode == None.
    std::vector<double> offsets;
    /// Post-anchor error per matched goal, (anchor + offset) − goalFrame*frameTime.
    /// ~0 by construction for PerGoal/LegacyEnd; meaningful in SingleOffset
    /// mode, where it exposes the cumulative cinematic divergence.
    std::vector<double> residuals;
    /// Human-readable summary of what was matched and why (for logs).
    std::string note;
};

/// Computes per-segment offsets. `goalFrames` is sorted internally; pass the
/// header's Goals[].frame values as-is. `frameTime` is seconds per replay
/// frame (1 / RecordFPS, typically 1/30).
AnchorPlan ComputeAnchorPlan(const std::vector<SegmentInfo>& segments,
                             std::vector<int> goalFrames,
                             double frameTime);

/// Shifts startTimeSec, endTimeSec, and goalTimeSec (when >= 0) of each
/// segment by its offset. No-op when plan.mode == None or sizes mismatch.
void ApplyAnchorPlan(std::vector<SegmentInfo>& segments, const AnchorPlan& plan);

/// One-line log form: "mode=per-goal offsets=[...] deltas=[...] residuals=[...] note=..."
std::string DescribeAnchorPlan(const AnchorPlan& plan);

} // namespace SyncComms
