#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>

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

    printf("\nAll tests passed!\n");
    return 0;
}
