#include "SyncComms/AudioResampler.h"
#include <cstring>
#include <cmath>
#include <algorithm>

namespace SyncComms {

void AudioResampler::Resample(const float* input, uint32_t inputFrameCount,
                              float* output, uint32_t outputFrameCount,
                              int channels, float ratio) {
    if (inputFrameCount == 0 || outputFrameCount == 0) {
        std::memset(output, 0, outputFrameCount * channels * sizeof(float));
        return;
    }

    for (uint32_t i = 0; i < outputFrameCount; i++) {
        double srcPos = m_fractionalPosition + i * static_cast<double>(ratio);
        uint32_t idx = static_cast<uint32_t>(srcPos);
        float frac = static_cast<float>(srcPos - idx);

        for (int ch = 0; ch < channels; ch++) {
            float s0 = 0.0f;
            float s1 = 0.0f;

            if (idx < inputFrameCount) {
                s0 = input[idx * channels + ch];
            }
            if (idx + 1 < inputFrameCount) {
                s1 = input[(idx + 1) * channels + ch];
            } else {
                s1 = s0;
            }

            output[i * channels + ch] = s0 + frac * (s1 - s0);
        }
    }

    // Track fractional position for continuity across callbacks
    m_fractionalPosition += outputFrameCount * static_cast<double>(ratio);
    // Keep only the fractional part relative to consumed input
    if (m_fractionalPosition >= static_cast<double>(inputFrameCount)) {
        m_fractionalPosition = std::fmod(m_fractionalPosition, static_cast<double>(inputFrameCount));
    }
}

void AudioResampler::Reset() {
    m_fractionalPosition = 0.0;
}

} // namespace SyncComms
