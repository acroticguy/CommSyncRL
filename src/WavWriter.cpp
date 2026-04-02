#include "SyncComms/WavWriter.h"
#include <cstring>

namespace SyncComms {

WavWriter::~WavWriter() {
    if (m_file.is_open()) {
        Close();
    }
}

bool WavWriter::Open(const std::string& filePath, int sampleRate, int channels, int bitsPerSample) {
    if (m_file.is_open()) {
        Close();
    }

    m_filePath = filePath;
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_bitsPerSample = bitsPerSample;
    m_dataBytes = 0;
    m_totalFramesWritten = 0;

    m_file.open(filePath, std::ios::binary | std::ios::trunc);
    if (!m_file.is_open()) {
        return false;
    }

    WriteHeader();
    return true;
}

void WavWriter::WriteSamples(const float* data, uint32_t frameCount) {
    if (!m_file.is_open() || frameCount == 0) return;

    uint32_t sampleCount = frameCount * m_channels;

    if (m_bitsPerSample == 32) {
        // Write 32-bit float samples directly
        uint32_t bytes = sampleCount * sizeof(float);
        m_file.write(reinterpret_cast<const char*>(data), bytes);
        m_dataBytes += bytes;
    } else if (m_bitsPerSample == 16) {
        // Convert float to 16-bit PCM
        for (uint32_t i = 0; i < sampleCount; i++) {
            float sample = data[i];
            if (sample > 1.0f) sample = 1.0f;
            if (sample < -1.0f) sample = -1.0f;
            int16_t pcm = static_cast<int16_t>(sample * 32767.0f);
            m_file.write(reinterpret_cast<const char*>(&pcm), sizeof(int16_t));
        }
        m_dataBytes += sampleCount * sizeof(int16_t);
    }

    m_totalFramesWritten += frameCount;
}

void WavWriter::Close() {
    if (!m_file.is_open()) return;

    PatchHeader();
    m_file.close();
}

void WavWriter::WriteHeader() {
    // Write a placeholder RIFF/WAV header (44 bytes)
    // Will be patched with correct sizes on Close()

    uint16_t audioFormat = (m_bitsPerSample == 32) ? 3 : 1; // 3 = IEEE float, 1 = PCM
    uint16_t numChannels = static_cast<uint16_t>(m_channels);
    uint32_t sampleRate = static_cast<uint32_t>(m_sampleRate);
    uint16_t bitsPerSample = static_cast<uint16_t>(m_bitsPerSample);
    uint16_t blockAlign = numChannels * (bitsPerSample / 8);
    uint32_t byteRate = sampleRate * blockAlign;

    // Placeholder sizes (patched on close)
    uint32_t riffChunkSize = 0xFFFFFFFF;
    uint32_t dataChunkSize = 0xFFFFFFFF;

    // RIFF header
    m_file.write("RIFF", 4);
    m_file.write(reinterpret_cast<const char*>(&riffChunkSize), 4);
    m_file.write("WAVE", 4);

    // fmt sub-chunk
    m_file.write("fmt ", 4);
    uint32_t fmtChunkSize = 16;
    m_file.write(reinterpret_cast<const char*>(&fmtChunkSize), 4);
    m_file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    m_file.write(reinterpret_cast<const char*>(&numChannels), 2);
    m_file.write(reinterpret_cast<const char*>(&sampleRate), 4);
    m_file.write(reinterpret_cast<const char*>(&byteRate), 4);
    m_file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    m_file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

    // data sub-chunk
    m_file.write("data", 4);
    m_file.write(reinterpret_cast<const char*>(&dataChunkSize), 4);
}

void WavWriter::PatchHeader() {
    if (!m_file.is_open()) return;

    // Patch RIFF chunk size (byte 4): fileSize - 8
    uint32_t riffSize = 36 + m_dataBytes;
    m_file.seekp(4, std::ios::beg);
    m_file.write(reinterpret_cast<const char*>(&riffSize), 4);

    // Patch data chunk size (byte 40)
    m_file.seekp(40, std::ios::beg);
    m_file.write(reinterpret_cast<const char*>(&m_dataBytes), 4);

    // Seek back to end
    m_file.seekp(0, std::ios::end);
}

} // namespace SyncComms
