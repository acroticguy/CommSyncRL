#pragma once

#include <string>
#include <fstream>
#include <cstdint>

namespace SyncComms {

/// Incremental WAV file writer. Writes RIFF header on open, streams PCM data,
/// and patches chunk sizes on close.
class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter();

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool Open(const std::string& filePath, int sampleRate, int channels, int bitsPerSample = 32);
    void WriteSamples(const float* data, uint32_t frameCount);
    void Close();

    bool IsOpen() const { return m_file.is_open(); }
    uint32_t GetTotalFramesWritten() const { return m_totalFramesWritten; }

private:
    void WriteHeader();
    void PatchHeader();

    std::ofstream m_file;
    std::string   m_filePath;
    int           m_sampleRate      = 48000;
    int           m_channels        = 1;
    int           m_bitsPerSample   = 32;
    uint32_t      m_dataBytes       = 0;
    uint32_t      m_totalFramesWritten = 0;
};

} // namespace SyncComms
