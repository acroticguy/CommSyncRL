#include "SyncComms/ReplayLocator.h"
#include "SyncComms/ReplayMetadata.h"

#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace SyncComms {

namespace {

std::filesystem::path DefaultDemosDir() {
    PWSTR raw = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw);
    if (FAILED(hr) || !raw) {
        if (raw) CoTaskMemFree(raw);
        return {};
    }
    std::filesystem::path p(raw);
    CoTaskMemFree(raw);
    p /= L"My Games";
    p /= L"Rocket League";
    p /= L"TAGame";
    p /= L"Demos";
    return p;
}

} // namespace

ReplayLocator::ReplayLocator()
    : m_demosDir(DefaultDemosDir())
{
}

void ReplayLocator::SetDemosDir(std::filesystem::path dir) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_demosDir = std::move(dir);
    m_guidToPath.clear();
    m_byEpoch.clear();
    m_seen.clear();
}

bool ReplayLocator::DemosDirAvailable() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::error_code ec;
    return !m_demosDir.empty() && std::filesystem::exists(m_demosDir, ec);
}

std::optional<std::filesystem::path> ReplayLocator::FindByMatchGuid(const std::string& guid) {
    if (guid.empty()) return std::nullopt;

    std::lock_guard<std::mutex> lock(m_mutex);

    // Fast path: cached.
    auto it = m_guidToPath.find(guid);
    if (it != m_guidToPath.end() && std::filesystem::exists(it->second)) {
        return it->second;
    }

    Rescan();

    it = m_guidToPath.find(guid);
    if (it != m_guidToPath.end()) return it->second;
    return std::nullopt;
}

std::optional<std::filesystem::path> ReplayLocator::FindByEpoch(int64_t targetEpoch,
                                                                double toleranceSec) {
    if (targetEpoch <= 0) return std::nullopt;

    std::lock_guard<std::mutex> lock(m_mutex);
    Rescan();

    const std::filesystem::path* best = nullptr;
    double bestDist = toleranceSec;
    for (const auto& info : m_byEpoch) {
        if (info.matchStartEpoch <= 0) continue;
        const double startDist =
            std::abs(static_cast<double>(info.matchStartEpoch - targetEpoch));
        const double endDist = std::abs(
            static_cast<double>(info.matchStartEpoch) + info.totalSecondsPlayed -
            static_cast<double>(targetEpoch));
        const double dist = std::min(startDist, endDist);
        if (dist <= bestDist) {
            bestDist = dist;
            best = &info.path;
        }
    }
    if (best) return *best;
    return std::nullopt;
}

void ReplayLocator::Rescan() {
    std::error_code ec;
    if (!std::filesystem::exists(m_demosDir, ec)) return;

    // Collect candidate paths first so we can sort newest-first; the user is
    // most likely opening their most-recent replay, so this short-circuits the
    // common case and keeps cold parses to a minimum.
    struct Candidate {
        std::filesystem::path                  path;
        std::filesystem::file_time_type        mtime;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(64);

    for (const auto& entry : std::filesystem::directory_iterator(m_demosDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".replay") continue;

        auto t = std::filesystem::last_write_time(entry.path(), ec);
        if (ec) { ec.clear(); continue; }
        candidates.push_back({entry.path(), t});
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.mtime > b.mtime;
              });

    for (const auto& c : candidates) {
        // Skip files we've already parsed AND whose mtime hasn't changed.
        auto seenIt = m_seen.find(c.path);
        if (seenIt != m_seen.end() && seenIt->second == c.mtime) continue;

        ReplayMetadata meta;
        if (ParseReplayHeader(c.path, meta)) {
            if (!meta.matchGuid.empty()) {
                m_guidToPath[meta.matchGuid] = c.path;  // legacy / future formats
            }
            if (meta.matchStartEpoch > 0) {
                m_byEpoch.push_back({c.path, meta.matchStartEpoch,
                                     meta.totalSecondsPlayed});
            }
        }
        m_seen[c.path] = c.mtime;
    }
}

} // namespace SyncComms
