#include "SyncComms/RecordingMaintenance.h"
#include "SyncComms/ReplayLocator.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace SyncComms {

namespace {

void Notify(const MaintenanceLogFn& log, const std::string& msg) {
    if (log) log(msg);
}

bool IsSidecar(const fs::path& p) {
    return p.extension() == ".json" &&
           p.stem().u8string().find("_synccomms") != std::string::npos;
}

// Best-effort parse of a sidecar file into a json object. Returns false (and
// leaves `out` untouched) on any open/parse error.
bool LoadJson(const fs::path& p, json& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    try {
        in >> out;
    } catch (...) {
        return false;
    }
    return true;
}

// Whole days between a file's last-write time and now. Uses the same
// file_time → system_clock bridge as the recordings listing so the age the
// retention pass sees matches what the UI shows.
double FileAgeDays(const fs::path& p) {
    std::error_code ec;
    auto ftime = fs::last_write_time(p, ec);
    if (ec) return 0.0;
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    auto age = std::chrono::system_clock::now() - sctp;
    return std::chrono::duration<double>(age).count() / 86400.0;
}

// True if `id` is safe to splice into a filename (no traversal / separators).
bool IsSafeId(const std::string& id) {
    return !id.empty() &&
           id.find('/') == std::string::npos &&
           id.find('\\') == std::string::npos &&
           id.find("..") == std::string::npos;
}

// Remove the WAV files a sidecar still references (normally none after a clean
// finalize). `outputDir` carries a trailing slash per StandaloneConfig.
void RemoveReferencedWavs(const std::string& outputDir, const json& side,
                          const MaintenanceLogFn& log) {
    if (!side.contains("segments") || !side["segments"].is_array()) return;
    for (const auto& s : side["segments"]) {
        const std::string af = s.value("audioFile", std::string{});
        if (af.empty()) continue;
        std::error_code ec;
        if (fs::remove(fs::path(outputDir + af), ec)) {
            Notify(log, "[maintenance] removed referenced WAV: " + af);
        }
    }
}

} // namespace

int SweepOrphanWavs(const std::string& outputDir, const MaintenanceLogFn& log) {
    std::error_code ec;
    if (outputDir.empty() || !fs::exists(outputDir, ec)) return 0;

    // Pass 1: collect WAVs that are some segment's ONLY copy (referenced by a
    // sidecar but not yet embedded as audioData). Those must survive the sweep.
    std::set<std::string> soleCopy;
    for (const auto& entry : fs::directory_iterator(outputDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file() || !IsSidecar(entry.path())) continue;
        json side;
        if (!LoadJson(entry.path(), side)) continue;
        if (!side.contains("segments") || !side["segments"].is_array()) continue;
        for (const auto& s : side["segments"]) {
            const std::string af = s.value("audioFile", std::string{});
            const std::string ad = s.value("audioData", std::string{});
            if (!af.empty() && ad.empty()) soleCopy.insert(af);
        }
    }

    // Pass 2: delete every other WAV (embedded-redundant or truly orphaned).
    int deleted = 0;
    for (const auto& entry : fs::directory_iterator(outputDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".wav") continue;
        const std::string name = entry.path().filename().u8string();
        if (soleCopy.count(name)) continue;  // sole on-disk copy — keep
        std::error_code rec;
        if (fs::remove(entry.path(), rec)) {
            ++deleted;
            Notify(log, "[maintenance] swept orphan WAV: " + name);
        }
    }

    if (deleted > 0) {
        Notify(log, "[maintenance] orphan sweep removed " +
                        std::to_string(deleted) + " WAV file(s)");
    }
    return deleted;
}

int PruneUnboundSidecars(const std::string& outputDir, ReplayLocator& locator,
                         double maxAgeDays, const MaintenanceLogFn& log) {
    std::error_code ec;
    if (outputDir.empty() || !fs::exists(outputDir, ec)) return 0;

    // Safety valve: if RL's demos directory is gone (uninstalled / moved),
    // boundness can't be judged — refuse to prune so we never wipe a library
    // just because the replay folder relocated.
    if (!locator.DemosDirAvailable()) {
        Notify(log, "[maintenance] retention skipped — RL demos dir unavailable");
        return 0;
    }

    // Snapshot candidates first; we mutate the directory as we delete.
    std::vector<fs::path> sidecars;
    for (const auto& entry : fs::directory_iterator(outputDir, ec)) {
        if (ec) break;
        if (entry.is_regular_file() && IsSidecar(entry.path())) {
            sidecars.push_back(entry.path());
        }
    }

    int deleted = 0;
    for (const auto& path : sidecars) {
        const double ageDays = FileAgeDays(path);
        if (ageDays <= maxAgeDays) continue;  // within retention window — keep

        json side;
        if (!LoadJson(path, side)) continue;  // unreadable — leave it alone

        const std::string replayPath = side.value("replayPath", std::string{});
        const int64_t epoch = side.value("matchStartEpoch", static_cast<int64_t>(0));

        bool bound = false;
        if (!replayPath.empty() && fs::exists(fs::path(replayPath), ec)) {
            bound = true;
        } else if (epoch > 0 && locator.FindByEpoch(epoch).has_value()) {
            bound = true;
        }

        if (!ShouldPruneUnbound(bound, ageDays, maxAgeDays)) continue;

        RemoveReferencedWavs(outputDir, side, log);
        std::error_code rec;
        if (fs::remove(path, rec)) {
            ++deleted;
            Notify(log, "[maintenance] pruned unbound recording " +
                            path.filename().u8string() + " (age " +
                            std::to_string(static_cast<int>(ageDays)) + "d)");
        }
    }

    if (deleted > 0) {
        Notify(log, "[maintenance] retention removed " +
                        std::to_string(deleted) + " unbound recording(s)");
    }
    return deleted;
}

bool DeleteRecording(const std::string& outputDir, const std::string& matchGuid,
                     const MaintenanceLogFn& log) {
    if (outputDir.empty() || !IsSafeId(matchGuid)) {
        Notify(log, "[maintenance] deleteRecording rejected id: " + matchGuid);
        return false;
    }

    std::error_code ec;
    fs::path dir(outputDir);

    // Preferred path: <matchGuid>_synccomms.json (how SidecarManager names it).
    fs::path target = dir / (matchGuid + "_synccomms.json");
    if (!fs::exists(target, ec)) {
        // Fallback: a sidecar whose internal replayId matches (covers files
        // whose on-disk name drifted from the embedded id).
        target.clear();
        for (const auto& entry : fs::directory_iterator(outputDir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file() || !IsSidecar(entry.path())) continue;
            json side;
            if (!LoadJson(entry.path(), side)) continue;
            if (side.value("replayId", std::string{}) == matchGuid) {
                target = entry.path();
                break;
            }
        }
    }

    if (target.empty() || !fs::exists(target, ec)) {
        Notify(log, "[maintenance] deleteRecording: not found " + matchGuid);
        return false;
    }

    json side;
    if (LoadJson(target, side)) {
        RemoveReferencedWavs(outputDir, side, log);
    }

    std::error_code rec;
    if (!fs::remove(target, rec)) {
        Notify(log, "[maintenance] deleteRecording: remove failed " +
                        target.filename().u8string());
        return false;
    }
    Notify(log, "[maintenance] deleted recording " + target.filename().u8string());
    return true;
}

} // namespace SyncComms
