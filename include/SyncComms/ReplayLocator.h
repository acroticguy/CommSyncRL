#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace SyncComms {

/// Locates the saved .replay file on disk that corresponds to a match we
/// captured. Current RL replay headers carry NO MatchGuid, so matching is done
/// by time: the header's MatchStartEpoch (Unix seconds at match start) and
/// TotalSecondsPlayed (match length) are compared against a target epoch from
/// the capture side. Caches parsed headers so we only parse each .replay once
/// per session (or until its mtime changes).
///
/// Default search root is %USERPROFILE%/Documents/My Games/Rocket League/
/// TAGame/Demos/, where Rocket League writes .replay files.
class ReplayLocator {
public:
    ReplayLocator();

    /// Find the .replay file whose header property MatchGuid matches `guid`.
    /// LEGACY: current RL replays have no MatchGuid so this almost always
    /// returns nullopt — kept for older replays / future format changes.
    std::optional<std::filesystem::path> FindByMatchGuid(const std::string& guid);

    /// Find the .replay whose match time is closest to `targetEpoch` (Unix
    /// seconds). Distance is the smaller of |start - target| and
    /// |start + totalSeconds - target|, so the caller may pass either a
    /// match-START epoch (new captures) or a match-END epoch (legacy sidecars,
    /// whose only timestamp is the finalize/captureDate time). Returns nullopt
    /// if nothing is within `toleranceSec`.
    std::optional<std::filesystem::path> FindByEpoch(int64_t targetEpoch,
                                                     double toleranceSec = 180.0);

    /// Override the demos directory (mainly for tests).
    void SetDemosDir(std::filesystem::path dir);

    /// True if the configured demos directory exists on disk. Used by the
    /// retention pass as a safety valve: when RL's replay folder is missing
    /// (uninstalled / moved), boundness is unknowable, so pruning is skipped.
    bool DemosDirAvailable();

private:
    void Rescan();

    struct ReplayInfo {
        std::filesystem::path path;
        int64_t matchStartEpoch = 0;
        double  totalSecondsPlayed = 0.0;
    };

    std::filesystem::path m_demosDir;
    std::map<std::string, std::filesystem::path> m_guidToPath;
    std::vector<ReplayInfo> m_byEpoch;
    std::map<std::filesystem::path, std::filesystem::file_time_type> m_seen;
    std::mutex m_mutex;
};

} // namespace SyncComms
