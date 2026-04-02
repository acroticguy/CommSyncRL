#pragma once

#include <cstdint>
#include <vector>

namespace SyncComms {

/// Simple linear-interpolation resampler for time-dilated playback.
/// Converts inputFrameCount source frames into outputFrameCount output frames
/// using the given speed ratio (dilation).
class AudioResampler {
public:
    AudioResampler() = default;

    /// Resample input to output with given speed ratio.
    /// ratio > 1.0 = faster (consumes more input per output frame)
    /// ratio < 1.0 = slower (consumes less input per output frame)
    void Resample(const float* input, uint32_t inputFrameCount,
                  float* output, uint32_t outputFrameCount,
                  int channels, float ratio);

    /// Reset internal state (call after seek)
    void Reset();

private:
    double m_fractionalPosition = 0.0;
};

} // namespace SyncComms
