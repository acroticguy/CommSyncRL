#pragma once

#include <cmath>

namespace SyncComms {

/// Detects a stalled (paused) replay clock from repeated identical time
/// samples. Time-based rather than call-count-based, so it behaves the same
/// whether the caller ticks at BakkesMod's ~120Hz game tick or the Stats
/// API's PacketSendRate (typically 30Hz).
struct ReplayStallDetector {
    /// ~300ms matches the old 30-stale-ticks heuristic at 120Hz.
    double thresholdSec = 0.3;

    /// `replayTimeSec` is the latest replay clock sample; `nowSec` is a
    /// monotonic wall-clock reading (injected so tests can drive it).
    /// Returns true while the replay clock has been stalled for longer than
    /// thresholdSec; resumes (returns false) on the first sample that moves.
    bool Update(float replayTimeSec, double nowSec) {
        if (m_lastProgressAt < 0.0 ||
            std::abs(replayTimeSec - m_lastReplayTime) >= 0.001f) {
            m_lastReplayTime = replayTimeSec;
            m_lastProgressAt = nowSec;
            return false;
        }
        return (nowSec - m_lastProgressAt) > thresholdSec;
    }

    void Reset() {
        m_lastReplayTime = -1.0f;
        m_lastProgressAt = -1.0;
    }

private:
    float  m_lastReplayTime = -1.0f;
    double m_lastProgressAt = -1.0;
};

} // namespace SyncComms
