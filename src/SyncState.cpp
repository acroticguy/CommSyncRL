#include "SyncComms/SyncState.h"

namespace SyncComms {

void SyncState::SetSegments(const std::vector<SegmentInfo>& segs) {
    std::lock_guard<std::mutex> lock(m_segmentMutex);
    m_segments = segs;
}

std::vector<SegmentInfo> SyncState::GetSegments() const {
    std::lock_guard<std::mutex> lock(m_segmentMutex);
    return m_segments;
}

SegmentInfo SyncState::GetSegment(int index) const {
    std::lock_guard<std::mutex> lock(m_segmentMutex);
    return m_segments.at(index);
}

int SyncState::GetSegmentCount() const {
    std::lock_guard<std::mutex> lock(m_segmentMutex);
    return static_cast<int>(m_segments.size());
}

void SyncState::Reset() {
    currentReplayFrame.store(0);
    timeDilation.store(1.0f);
    isReplayPaused.store(false);
    isInReplay.store(false);
    isRecording.store(false);
    isCapturingSegment.store(false);
    requestSeek.store(false);
    seekTargetFrame.store(0);
    latencyOffsetMs.store(0.0f);
    volumeMultiplier.store(1.0f);
    activeSegmentIndex.store(-1);
    currentAudioSample.store(0);
    currentDriftMs.store(0.0f);

    std::lock_guard<std::mutex> lock(m_segmentMutex);
    m_segments.clear();
}

} // namespace SyncComms
