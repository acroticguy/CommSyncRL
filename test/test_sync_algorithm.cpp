#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>

// Real production code under test (pure, no BakkesMod/miniaudio deps).
#include "SyncComms/SegmentAnchoring.h"
#include "SyncComms/ReplayStallDetector.h"
#include "SyncComms/ReplayMetadata.h"
#include "SyncComms/RecordingMaintenance.h"
#include <cstdlib>
#include <filesystem>

// Minimal reimplementation of sync math for testing (no BakkesMod/miniaudio deps)

struct SegmentInfo {
    int    index;
    int    startFrame;
    int    endFrame;
    double frameTime;
    std::string audioFile;
};

// === Core sync formula ===
int64_t ComputeTargetSample(int replayFrame, const SegmentInfo& seg,
                            float latencyOffsetMs, int sampleRate) {
    double elapsedFrames = static_cast<double>(replayFrame - seg.startFrame);
    double audioTimeSec = elapsedFrames * seg.frameTime;
    audioTimeSec += latencyOffsetMs / 1000.0;
    if (audioTimeSec < 0.0) audioTimeSec = 0.0;
    return static_cast<int64_t>(audioTimeSec * sampleRate);
}

// === Active segment selection ===
int FindActiveSegment(int currentFrame, const std::vector<SegmentInfo>& segments) {
    for (int i = 0; i < static_cast<int>(segments.size()); i++) {
        if (currentFrame >= segments[i].startFrame &&
            currentFrame <= segments[i].endFrame) {
            return i;
        }
    }
    return -1;
}

// === Scrub detection ===
bool IsScrub(int newFrame, int oldFrame, int threshold = 5) {
    return std::abs(newFrame - oldFrame) > threshold;
}

// === Pause detection ===
struct PauseDetector {
    int lastFrame = -1;
    int unchangedCount = 0;
    int threshold = 3;

    bool Update(int frame) {
        if (frame == lastFrame) {
            unchangedCount++;
            return unchangedCount > threshold;
        }
        unchangedCount = 0;
        lastFrame = frame;
        return false;
    }
};

// === Linear interpolation resampler ===
void Resample(const float* input, int inputCount,
              float* output, int outputCount,
              float ratio) {
    for (int i = 0; i < outputCount; i++) {
        double srcPos = i * static_cast<double>(ratio);
        int idx = static_cast<int>(srcPos);
        float frac = static_cast<float>(srcPos - idx);
        float s0 = (idx < inputCount) ? input[idx] : 0.0f;
        float s1 = (idx + 1 < inputCount) ? input[idx + 1] : s0;
        output[i] = s0 + frac * (s1 - s0);
    }
}

// === Tests ===

void test_compute_target_sample() {
    SegmentInfo seg;
    seg.startFrame = 150;
    seg.endFrame = 4230;
    seg.frameTime = 1.0 / 30.0;

    // At startFrame, audio position should be 0
    int64_t sample = ComputeTargetSample(150, seg, 0.0f, 48000);
    assert(sample == 0);

    // 30 frames later = 1 second = 48000 samples
    sample = ComputeTargetSample(180, seg, 0.0f, 48000);
    assert(sample == 48000);

    // 300 frames = 10 seconds
    sample = ComputeTargetSample(450, seg, 0.0f, 48000);
    assert(sample == 480000);

    // With positive latency offset (+100ms = +4800 samples)
    sample = ComputeTargetSample(150, seg, 100.0f, 48000);
    assert(sample == 4800);

    // With negative latency offset at start (clamped to 0)
    sample = ComputeTargetSample(150, seg, -100.0f, 48000);
    assert(sample == 0);

    // Negative offset mid-segment
    sample = ComputeTargetSample(180, seg, -500.0f, 48000);
    // 1.0 - 0.5 = 0.5s = 24000 samples
    assert(sample == 24000);

    printf("  [PASS] test_compute_target_sample\n");
}

void test_find_active_segment() {
    std::vector<SegmentInfo> segments = {
        {0, 150,  4230, 1.0/30.0, "seg000.wav"},
        {1, 4500, 8100, 1.0/30.0, "seg001.wav"},
        {2, 8350, 12600, 1.0/30.0, "seg002.wav"},
    };

    // In first segment
    assert(FindActiveSegment(150, segments) == 0);
    assert(FindActiveSegment(2000, segments) == 0);
    assert(FindActiveSegment(4230, segments) == 0);

    // Between segments
    assert(FindActiveSegment(4300, segments) == -1);

    // In second segment
    assert(FindActiveSegment(4500, segments) == 1);
    assert(FindActiveSegment(6000, segments) == 1);

    // In third segment
    assert(FindActiveSegment(10000, segments) == 2);

    // Before all segments
    assert(FindActiveSegment(0, segments) == -1);

    // After all segments
    assert(FindActiveSegment(20000, segments) == -1);

    printf("  [PASS] test_find_active_segment\n");
}

void test_scrub_detection() {
    assert(!IsScrub(101, 100, 5));   // 1 frame = not a scrub
    assert(!IsScrub(105, 100, 5));   // exactly threshold = not a scrub
    assert(IsScrub(106, 100, 5));    // 6 frames = scrub
    assert(IsScrub(50, 100, 5));     // backwards scrub
    assert(!IsScrub(100, 100, 5));   // same frame

    printf("  [PASS] test_scrub_detection\n");
}

void test_pause_detection() {
    PauseDetector pd;
    pd.threshold = 3;

    assert(!pd.Update(100)); // first frame
    assert(!pd.Update(101)); // advancing
    assert(!pd.Update(101)); // 1 unchanged
    assert(!pd.Update(101)); // 2 unchanged
    assert(!pd.Update(101)); // 3 unchanged
    assert(pd.Update(101));  // 4 unchanged -> paused!
    assert(!pd.Update(102)); // resumed

    printf("  [PASS] test_pause_detection\n");
}

void test_resampler_identity() {
    // At ratio 1.0, output should equal input
    float input[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    float output[5];
    Resample(input, 5, output, 5, 1.0f);

    for (int i = 0; i < 5; i++) {
        assert(std::abs(output[i] - input[i]) < 0.001f);
    }

    printf("  [PASS] test_resampler_identity\n");
}

void test_resampler_2x_speed() {
    // At ratio 2.0, we read 2x input frames per output frame
    // Input: 0, 1, 2, 3, 4, 5, 6, 7 (8 frames)
    // Output: 4 frames at positions 0, 2, 4, 6
    float input[] = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
    float output[4];
    Resample(input, 8, output, 4, 2.0f);

    assert(std::abs(output[0] - 0.0f) < 0.001f);
    assert(std::abs(output[1] - 2.0f) < 0.001f);
    assert(std::abs(output[2] - 4.0f) < 0.001f);
    assert(std::abs(output[3] - 6.0f) < 0.001f);

    printf("  [PASS] test_resampler_2x_speed\n");
}

void test_resampler_half_speed() {
    // At ratio 0.5, we read 0.5x input frames per output frame
    // Input: 0, 2, 4, 6 (4 frames)
    // Output: 8 frames (interpolated between input samples)
    float input[] = {0.0f, 2.0f, 4.0f, 6.0f};
    float output[8];
    Resample(input, 4, output, 8, 0.5f);

    assert(std::abs(output[0] - 0.0f) < 0.001f);
    assert(std::abs(output[1] - 1.0f) < 0.001f);  // interpolated
    assert(std::abs(output[2] - 2.0f) < 0.001f);
    assert(std::abs(output[3] - 3.0f) < 0.001f);  // interpolated
    assert(std::abs(output[4] - 4.0f) < 0.001f);
    assert(std::abs(output[5] - 5.0f) < 0.001f);  // interpolated
    assert(std::abs(output[6] - 6.0f) < 0.001f);

    printf("  [PASS] test_resampler_half_speed\n");
}

void test_full_match_sync_accuracy() {
    // Simulate a full 5-minute match segment at 30fps replay
    SegmentInfo seg;
    seg.startFrame = 100;
    seg.endFrame = 9100;  // 300 seconds * 30fps = 9000 frames
    seg.frameTime = 1.0 / 30.0;

    int sampleRate = 48000;

    // Check sync at various points throughout the match
    struct TestPoint { int frame; double expectedSec; };
    TestPoint points[] = {
        {100,   0.0},
        {400,   10.0},
        {1600,  50.0},
        {4600,  150.0},
        {9100,  300.0},
    };

    for (const auto& pt : points) {
        int64_t sample = ComputeTargetSample(pt.frame, seg, 0.0f, sampleRate);
        double actualSec = static_cast<double>(sample) / sampleRate;
        double error = std::abs(actualSec - pt.expectedSec);
        // Must be within 1ms accuracy
        assert(error < 0.001);
    }

    printf("  [PASS] test_full_match_sync_accuracy\n");
}

// === Segment anchoring (SyncComms/SegmentAnchoring.h) ===

namespace {

const double kFt = 1.0 / 30.0;  // frameTime used throughout anchoring tests

SyncComms::SegmentInfo MakeSeg(int index, double start, double end,
                               double goal = -1.0) {
    SyncComms::SegmentInfo s{};
    s.index = index;
    s.startTimeSec = start;
    s.endTimeSec = end;
    s.goalTimeSec = goal;
    s.frameTime = kFt;
    return s;
}

bool Near(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) < eps;
}

} // namespace

void test_anchor_per_goal_equal_counts() {
    // Replay goals at 55s, 120s, 180s. Capture clock lags by growing
    // cinematic time: offsets should come out 1.0, 9.0, 16.0.
    std::vector<SyncComms::SegmentInfo> segs = {
        MakeSeg(0,   4.0,  57.5,  54.0),   // goal at replay 55s  -> offset  1.0
        MakeSeg(1,  64.0, 114.5, 111.0),   // goal at replay 120s -> offset  9.0
        MakeSeg(2, 122.0, 167.5, 164.0),   // goal at replay 180s -> offset 16.0
    };
    auto plan = SyncComms::ComputeAnchorPlan(segs, {1650, 3600, 5400}, kFt);

    assert(plan.mode == SyncComms::AnchorMode::PerGoal);
    assert(plan.offsets.size() == 3);
    assert(Near(plan.offsets[0], 1.0));
    assert(Near(plan.offsets[1], 9.0));
    assert(Near(plan.offsets[2], 16.0));
    for (double r : plan.residuals) assert(Near(r, 0.0, 1e-9));
    // Offsets only grow within a match (cinematics add wall-clock time).
    for (size_t i = 1; i < plan.offsets.size(); ++i) {
        assert(plan.offsets[i] >= plan.offsets[i - 1]);
    }

    SyncComms::ApplyAnchorPlan(segs, plan);
    assert(Near(segs[0].goalTimeSec, 55.0));
    assert(Near(segs[1].goalTimeSec, 120.0));
    assert(Near(segs[2].goalTimeSec, 180.0));
    assert(Near(segs[0].startTimeSec, 5.0));

    printf("  [PASS] test_anchor_per_goal_equal_counts\n");
}

void test_anchor_trailing_unstamped_segment() {
    // Match ended on the timer: last segment has no goal stamp and must
    // inherit the previous segment's offset.
    std::vector<SyncComms::SegmentInfo> segs = {
        MakeSeg(0,   4.0,  57.5,  54.0),   // -> offset 1.0
        MakeSeg(1,  64.0, 114.5, 111.0),   // -> offset 9.0
        MakeSeg(2, 122.0, 200.0),          // no goal -> inherits 9.0
    };
    auto plan = SyncComms::ComputeAnchorPlan(segs, {1650, 3600}, kFt);

    assert(plan.mode == SyncComms::AnchorMode::PerGoal);
    assert(Near(plan.offsets[0], 1.0));
    assert(Near(plan.offsets[1], 9.0));
    assert(Near(plan.offsets[2], 9.0));

    SyncComms::ApplyAnchorPlan(segs, plan);
    assert(Near(segs[2].startTimeSec, 131.0));
    assert(Near(segs[2].goalTimeSec, -1.0));  // unstamped stays unstamped

    printf("  [PASS] test_anchor_trailing_unstamped_segment\n");
}

void test_anchor_no_goals() {
    std::vector<SyncComms::SegmentInfo> segs = {
        MakeSeg(0, 4.0, 200.0),
    };
    auto plan = SyncComms::ComputeAnchorPlan(segs, {}, kFt);
    assert(plan.mode == SyncComms::AnchorMode::None);

    SyncComms::ApplyAnchorPlan(segs, plan);  // must be a no-op
    assert(Near(segs[0].startTimeSec, 4.0));

    printf("  [PASS] test_anchor_no_goals\n");
}

void test_anchor_legacy_end_mode() {
    // Pre-goal-stamp sidecar: no goalTimeSec anywhere. Each segment END is
    // anchored to its goal frame — offsets are per-segment, not uniform.
    std::vector<SyncComms::SegmentInfo> segs = {
        MakeSeg(0,  4.0,  57.5),    // goal at 55s  -> offset -2.5
        MakeSeg(1, 64.0, 114.0),    // goal at 120s -> offset  6.0
    };
    auto plan = SyncComms::ComputeAnchorPlan(segs, {1650, 3600}, kFt);

    assert(plan.mode == SyncComms::AnchorMode::LegacyEnd);
    assert(Near(plan.offsets[0], -2.5));
    assert(Near(plan.offsets[1], 6.0));
    for (double r : plan.residuals) assert(Near(r, 0.0, 1e-9));

    printf("  [PASS] test_anchor_legacy_end_mode\n");
}

void test_anchor_legacy_trailing_segment() {
    // Legacy sidecar where the match ended on the timer: one more segment
    // than goals. The trailing segment is goal-less; it inherits.
    std::vector<SyncComms::SegmentInfo> segs = {
        MakeSeg(0,   4.0,  57.5),
        MakeSeg(1,  64.0, 114.0),
        MakeSeg(2, 122.0, 199.0),
    };
    auto plan = SyncComms::ComputeAnchorPlan(segs, {1650, 3600}, kFt);

    assert(plan.mode == SyncComms::AnchorMode::LegacyEnd);
    assert(Near(plan.offsets[0], -2.5));
    assert(Near(plan.offsets[1], 6.0));
    assert(Near(plan.offsets[2], 6.0));  // inherited

    printf("  [PASS] test_anchor_legacy_trailing_segment\n");
}

void test_anchor_missing_early_segments() {
    // Capture started mid-match: 2 stamped segments, 4 goals. Goal spacings
    // (195s, 70s, 20s) make only the middle window consistent with the
    // stamps' 58s spacing — steps of 137s and -38s are rejected as
    // implausible cinematic gaps.
    std::vector<SyncComms::SegmentInfo> segs = {
        MakeSeg(0,  50.0, 103.0, 100.0),
        MakeSeg(1, 110.0, 161.0, 158.0),
    };
    // goalSec = 55, 250, 320, 340
    auto plan = SyncComms::ComputeAnchorPlan(segs, {1650, 7500, 9600, 10200}, kFt);

    assert(plan.mode == SyncComms::AnchorMode::PerGoal);
    assert(Near(plan.offsets[0], 150.0));  // matched goals[1] (250s)
    assert(Near(plan.offsets[1], 162.0));  // matched goals[2] (320s)
    for (double r : plan.residuals) assert(Near(r, 0.0, 1e-9));

    printf("  [PASS] test_anchor_missing_early_segments\n");
}

void test_anchor_ambiguous_falls_back_single_offset() {
    // Evenly spaced goals (65s apart) make every window equally plausible
    // for the stamps' 58s spacing — must degrade to one offset taken from
    // the last stamp / last goal pair.
    std::vector<SyncComms::SegmentInfo> segs = {
        MakeSeg(0,  50.0, 103.0, 100.0),
        MakeSeg(1, 110.0, 161.0, 158.0),
    };
    // goalSec = 55, 120, 185
    auto plan = SyncComms::ComputeAnchorPlan(segs, {1650, 3600, 5550}, kFt);

    assert(plan.mode == SyncComms::AnchorMode::SingleOffset);
    assert(Near(plan.offsets[0], 185.0 - 158.0));  // 27.0 from last pair
    assert(Near(plan.offsets[1], 27.0));

    printf("  [PASS] test_anchor_ambiguous_falls_back_single_offset\n");
}

void test_anchor_more_stamps_than_goals() {
    // Header parse lost goals: 3 stamped segments, 1 goal. Degrade to a
    // single offset from the first pair.
    std::vector<SyncComms::SegmentInfo> segs = {
        MakeSeg(0,   4.0,  57.5,  54.0),
        MakeSeg(1,  64.0, 114.5, 111.0),
        MakeSeg(2, 122.0, 167.5, 164.0),
    };
    auto plan = SyncComms::ComputeAnchorPlan(segs, {1650}, kFt);

    assert(plan.mode == SyncComms::AnchorMode::SingleOffset);
    for (double o : plan.offsets) assert(Near(o, 1.0));

    printf("  [PASS] test_anchor_more_stamps_than_goals\n");
}

void test_anchor_apply_shifts_goal_stamp() {
    std::vector<SyncComms::SegmentInfo> segs = {
        MakeSeg(0, 4.0, 57.5, 54.0),
    };
    auto plan = SyncComms::ComputeAnchorPlan(segs, {1650}, kFt);
    assert(plan.mode == SyncComms::AnchorMode::PerGoal);

    SyncComms::ApplyAnchorPlan(segs, plan);
    assert(Near(segs[0].startTimeSec, 5.0));
    assert(Near(segs[0].endTimeSec, 58.5));
    assert(Near(segs[0].goalTimeSec, 55.0));  // stamp shifted too

    printf("  [PASS] test_anchor_apply_shifts_goal_stamp\n");
}

// === Replay stall (pause) detection (SyncComms/ReplayStallDetector.h) ===

void test_stall_detector_time_based() {
    // 30Hz feed (Stats API PacketSendRate): pause must be detected after
    // ~300ms of wall time, not after a fixed call count.
    {
        SyncComms::ReplayStallDetector d;
        double now = 0.0;
        float t = 5.0f;
        // Advancing clock: never stalled.
        for (int i = 0; i < 30; ++i) {
            assert(!d.Update(t, now));
            now += 1.0 / 30.0;
            t += 1.0f / 30.0f;
        }
        // Clock freezes: stalled once >0.3s of wall time passes (~10 calls).
        int callsUntilStall = 0;
        while (!d.Update(t, now)) {
            now += 1.0 / 30.0;
            ++callsUntilStall;
            assert(callsUntilStall < 12);
        }
        assert(callsUntilStall >= 9);
        // First moving sample resumes immediately.
        assert(!d.Update(t + 0.033f, now));
    }
    // 120Hz feed (BakkesMod tick): same ~300ms wall-time behavior.
    {
        SyncComms::ReplayStallDetector d;
        double now = 0.0;
        float t = 5.0f;
        assert(!d.Update(t, now));
        int callsUntilStall = 0;
        while (!d.Update(t, now)) {
            now += 1.0 / 120.0;
            ++callsUntilStall;
            assert(callsUntilStall < 40);
        }
        assert(callsUntilStall >= 36);
    }

    printf("  [PASS] test_stall_detector_time_based\n");
}

// === Recording retention predicate (SyncComms/RecordingMaintenance.h) ===

void test_retention_predicate() {
    using SyncComms::ShouldPruneUnbound;
    const double kMax = 14.0;

    // Bound recordings are kept regardless of age.
    assert(!ShouldPruneUnbound(/*bound*/ true, /*age*/ 100.0, kMax));
    assert(!ShouldPruneUnbound(true, 0.0, kMax));

    // Unbound but still inside the window — kept.
    assert(!ShouldPruneUnbound(false, 0.0, kMax));
    assert(!ShouldPruneUnbound(false, 13.9, kMax));

    // Boundary: exactly maxAge is NOT yet prunable (strictly-greater rule);
    // a hair past it is.
    assert(!ShouldPruneUnbound(false, 14.0, kMax));
    assert(ShouldPruneUnbound(false, 14.001, kMax));

    // Unbound and well past the window — pruned.
    assert(ShouldPruneUnbound(false, 30.0, kMax));

    // Default threshold is the 2-week retention constant.
    assert(SyncComms::kRetentionDays == 14.0);
    assert(ShouldPruneUnbound(false, 15.0));
    assert(!ShouldPruneUnbound(false, 10.0));

    printf("  [PASS] test_retention_predicate\n");
}

// === Real .replay header parse (guarded; opt-in via env var) ===
// Set SYNCCOMMS_TEST_REPLAY=<path-to.replay> to run. Verifies the production
// C++ parser (ReplayMetadata.cpp) extracts goalFrames from a real file — the
// thing per-goal anchoring depends on. Skipped (PASS) when the env var is
// unset so the suite stays hermetic.
void test_real_replay_header_parse() {
    const char* path = std::getenv("SYNCCOMMS_TEST_REPLAY");
    if (!path || !*path) {
        printf("  [SKIP] test_real_replay_header_parse (set SYNCCOMMS_TEST_REPLAY)\n");
        return;
    }
    SyncComms::ReplayMetadata meta;
    bool ok = SyncComms::ParseReplayHeader(std::filesystem::path(path), meta);
    printf("    parsed=%d numFrames=%d goals=%zu matchStartEpoch=%lld "
           "totalSecondsPlayed=%.1f recordFps=%.2f\n",
           ok ? 1 : 0, meta.numFrames, meta.goalFrames.size(),
           (long long)meta.matchStartEpoch, meta.totalSecondsPlayed, meta.recordFps);
    if (!meta.goalFrames.empty()) {
        printf("    goalFrames: ");
        for (int f : meta.goalFrames) printf("%d ", f);
        printf("\n");
    }
    // The whole anchoring pipeline needs goalFrames; assert we got some.
    assert(!meta.goalFrames.empty());
    printf("  [PASS] test_real_replay_header_parse\n");
}

int main() {
    printf("SyncComms Sync Algorithm Tests\n");
    printf("==============================\n");

    test_compute_target_sample();
    test_find_active_segment();
    test_scrub_detection();
    test_pause_detection();
    test_resampler_identity();
    test_resampler_2x_speed();
    test_resampler_half_speed();
    test_full_match_sync_accuracy();

    test_anchor_per_goal_equal_counts();
    test_anchor_trailing_unstamped_segment();
    test_anchor_no_goals();
    test_anchor_legacy_end_mode();
    test_anchor_legacy_trailing_segment();
    test_anchor_missing_early_segments();
    test_anchor_ambiguous_falls_back_single_offset();
    test_anchor_more_stamps_than_goals();
    test_anchor_apply_shifts_goal_stamp();
    test_stall_detector_time_based();
    test_retention_predicate();
    test_real_replay_header_parse();

    printf("\nAll tests passed!\n");
    return 0;
}
