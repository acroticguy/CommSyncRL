#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace SyncComms {

class AudioCompressor {
public:
    /// Compress a WAV file to OGG Vorbis in memory.
    /// quality: 0.0 (~64kbps) to 1.0 (~500kbps). 0.3 is good for voice.
    static std::vector<uint8_t> CompressWavToOgg(const std::string& wavPath,
                                                  int sampleRate, int channels,
                                                  float quality = 0.3f);

    static std::string Base64Encode(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> Base64Decode(const std::string& encoded);
};

} // namespace SyncComms
