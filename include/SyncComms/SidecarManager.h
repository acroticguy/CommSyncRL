#pragma once

#include "SyncComms/SyncState.h"
#include "SyncComms/Config.h"
#include "bakkesmod/wrappers/cvarmanagerwrapper.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace SyncComms {

class SidecarManager {
public:
    SidecarManager(Config* config, std::shared_ptr<CVarManagerWrapper> cvarManager);

    /// Write sidecar JSON for a completed capture session.
    bool WriteSidecar(const std::string& replayId, const std::vector<SegmentInfo>& segments);

    /// Read segments from an existing sidecar JSON.
    std::vector<SegmentInfo> ReadSidecar(const std::string& filePath);

    /// Search for a sidecar file matching a replay ID.
    std::optional<std::string> FindSidecar(const std::string& replayId);

    /// Search for a sidecar whose segments contain the given frame.
    std::optional<std::string> FindSidecarByFrame(int replayFrame);

    /// Compress WAV segments to OGG Vorbis files. Updates audioFile references and
    /// rewrites the sidecar. Deletes WAV files on success.
    void CompressSegments(const std::string& replayId, std::vector<SegmentInfo>& segments);

private:
    std::string BuildSidecarPath(const std::string& replayId);
    Config* m_config;
    std::shared_ptr<CVarManagerWrapper> m_cvarManager;
};

} // namespace SyncComms
