#include "SyncComms/SidecarManager.h"
#include "SyncComms/AudioCompressor.h"
#include "json.hpp"
#include <fstream>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

namespace SyncComms {

SidecarManager::SidecarManager(Config* config, std::shared_ptr<CVarManagerWrapper> cvarManager)
    : m_config(config)
    , m_cvarManager(std::move(cvarManager))
{
}

bool SidecarManager::WriteSidecar(const std::string& replayId, const std::vector<SegmentInfo>& segments) {
    if (replayId.empty() || segments.empty()) return false;

    // Build timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &time);
    std::ostringstream timeStr;
    timeStr << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");

    // Build JSON
    json j;
    j["version"] = 1;
    j["replayId"] = replayId;
    j["captureDate"] = timeStr.str();
    j["frameTime"] = segments.empty() ? (1.0 / 30.0) : segments[0].frameTime;
    j["sampleRate"] = m_config->GetSampleRate();
    j["channels"] = m_config->GetChannels();

    json segsJson = json::array();
    for (const auto& seg : segments) {
        json s;
        s["index"] = seg.index;
        s["startFrame"] = seg.startFrame;
        s["endFrame"] = seg.endFrame;
        s["audioFile"] = seg.audioFile;
        s["event"] = seg.event;
        s["durationSeconds"] = (seg.endFrame - seg.startFrame) * seg.frameTime;
        s["startTimeSec"] = seg.startTimeSec;
        s["endTimeSec"] = seg.endTimeSec;
        if (!seg.audioData.empty()) {
            s["audioData"] = seg.audioData;
        }
        segsJson.push_back(s);
    }
    j["segments"] = segsJson;

    json meta;
    meta["pluginVersion"] = "1.0.0";
    j["metadata"] = meta;

    // Write to file
    std::string path = BuildSidecarPath(replayId);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << j.dump(4);
    return file.good();
}

std::vector<SegmentInfo> SidecarManager::ReadSidecar(const std::string& filePath) {
    std::vector<SegmentInfo> segments;

    std::ifstream file(filePath);
    if (!file.is_open()) {
        if (m_cvarManager) m_cvarManager->log("SyncComms: Failed to open sidecar: " + filePath);
        return segments;
    }

    json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        if (m_cvarManager) m_cvarManager->log("SyncComms: Failed to parse sidecar JSON: " + std::string(e.what()));
        return segments;
    }

    if (!j.contains("version") || j["version"].get<int>() != 1) {
        if (m_cvarManager) m_cvarManager->log("SyncComms: Unsupported sidecar version in " + filePath);
        return segments;
    }

    double globalFrameTime = j.value("frameTime", 1.0 / 30.0);

    if (!j.contains("segments") || !j["segments"].is_array()) {
        return segments;
    }

    for (const auto& s : j["segments"]) {
        SegmentInfo seg;
        seg.index      = s.value("index", 0);
        seg.startFrame = s.value("startFrame", 0);
        seg.endFrame   = s.value("endFrame", 0);
        seg.frameTime    = globalFrameTime;
        seg.startTimeSec = s.value("startTimeSec", 0.0);
        seg.endTimeSec   = s.value("endTimeSec", 0.0);
        seg.audioFile    = s.value("audioFile", "");
        seg.audioData    = s.value("audioData", "");
        seg.event        = s.value("event", "");
        segments.push_back(seg);
    }

    return segments;
}

std::optional<std::string> SidecarManager::FindSidecar(const std::string& replayId) {
    if (replayId.empty()) return std::nullopt;

    // 1. Try exact match by replay ID
    std::string path = BuildSidecarPath(replayId);
    if (std::filesystem::exists(path)) {
        return path;
    }

    // 2. Search all sidecars and match by replayId field inside the JSON
    std::string outputDir = m_config->GetOutputDir();
    if (!std::filesystem::exists(outputDir)) return std::nullopt;

    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (entry.path().extension() == ".json") {
            std::ifstream file(entry.path());
            if (!file.is_open()) continue;
            try {
                json j;
                file >> j;
                if (j.contains("replayId") && j["replayId"].get<std::string>() == replayId) {
                    return entry.path().string();
                }
            } catch (...) {
                continue;
            }
        }
    }

    // 3. No match found
    return std::nullopt;
}

std::optional<std::string> SidecarManager::FindSidecarByFrame(int /*replayFrame*/) {
    // Fallback: if there's exactly one sidecar in the output folder, use it.
    // This handles exhibition matches where the replay ID doesn't match.
    std::string outputDir = m_config->GetOutputDir();
    if (!std::filesystem::exists(outputDir)) return std::nullopt;

    std::vector<std::string> jsonFiles;
    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (entry.path().extension() == ".json") {
            jsonFiles.push_back(entry.path().string());
        }
    }

    if (jsonFiles.size() == 1) {
        return jsonFiles[0];
    }

    // Multiple sidecars — find the most recently modified one
    if (!jsonFiles.empty()) {
        std::string newest;
        std::filesystem::file_time_type newestTime{};
        for (const auto& f : jsonFiles) {
            auto t = std::filesystem::last_write_time(f);
            if (newest.empty() || t > newestTime) {
                newestTime = t;
                newest = f;
            }
        }
        return newest;
    }

    return std::nullopt;
}

void SidecarManager::CompressSegments(const std::string& replayId, std::vector<SegmentInfo>& segments) {
    std::string outputDir = m_config->GetOutputDir();
    int sampleRate = m_config->GetSampleRate();
    int channels = m_config->GetChannels();

    bool allSucceeded = true;

    for (auto& seg : segments) {
        std::string wavPath = outputDir + seg.audioFile;
        if (!std::filesystem::exists(wavPath)) {
            allSucceeded = false;
            continue;
        }

        // Compress WAV to OGG Vorbis
        auto oggData = AudioCompressor::CompressWavToOgg(wavPath, sampleRate, channels, 0.3f);
        if (oggData.empty()) {
            allSucceeded = false;
            continue;
        }

        // Embed as base64 in the segment
        seg.audioData = AudioCompressor::Base64Encode(oggData);

        if (m_cvarManager) {
            float ratio = static_cast<float>(std::filesystem::file_size(wavPath)) /
                          static_cast<float>(oggData.size());
            m_cvarManager->log("SyncComms: Compressed " + seg.audioFile +
                               " (" + std::to_string(ratio) + "x, embedded)");
        }
    }

    // Rewrite sidecar with embedded audio
    WriteSidecar(replayId, segments);

    // Delete WAV files on success
    if (allSucceeded) {
        for (const auto& seg : segments) {
            std::string wavPath = outputDir + seg.audioFile;
            std::filesystem::remove(wavPath);
        }
    }
}

std::string SidecarManager::BuildSidecarPath(const std::string& replayId) {
    return m_config->GetOutputDir() + replayId + "_synccomms.json";
}

} // namespace SyncComms
