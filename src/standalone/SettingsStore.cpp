#include "SyncComms/SettingsStore.h"

#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <fstream>
#include <sstream>
#include <stdexcept>

#pragma comment(lib, "shell32.lib")

namespace SyncComms {

namespace {

std::filesystem::path GetAppDataDir() {
    PWSTR raw = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw);
    if (FAILED(hr) || !raw) {
        if (raw) CoTaskMemFree(raw);
        return {};
    }
    std::filesystem::path p(raw);
    CoTaskMemFree(raw);
    return p;
}

} // namespace

std::filesystem::path SettingsStore::GetPath() {
    auto root = GetAppDataDir();
    if (root.empty()) return {};
    auto dir = root / L"SyncComms";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    return dir / L"settings.json";
}

std::string SettingsStore::Load() {
    auto path = GetPath();
    if (path.empty()) {
        throw std::runtime_error("SettingsStore::Load: cannot resolve %APPDATA%");
    }
    if (!std::filesystem::exists(path)) {
        return "null"; // Never saved → frontend treats as defaults
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("SettingsStore::Load: cannot open " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void SettingsStore::Save(const std::string& json) {
    auto path = GetPath();
    if (path.empty()) {
        throw std::runtime_error("SettingsStore::Save: cannot resolve %APPDATA%");
    }
    // Atomic write: write to a sibling .tmp, then rename over the target.
    auto tmp = path;
    tmp += L".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("SettingsStore::Save: cannot open " + tmp.string());
        }
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out) {
            throw std::runtime_error("SettingsStore::Save: write failed");
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // rename can fail if the target exists on some filesystems; remove + rename.
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            throw std::runtime_error("SettingsStore::Save: rename failed: " + ec.message());
        }
    }
}

} // namespace SyncComms
