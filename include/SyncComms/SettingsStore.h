#pragma once

#include <string>
#include <filesystem>

namespace SyncComms {

// Tiny JSON file persistence for the standalone app's user-facing settings.
//
// Storage location: %APPDATA%\SyncComms\settings.json (created on first save).
// We don't validate or shape the JSON here — the schema lives in the frontend
// (lib/types.ts → AppSettings) and the C++ side just reads/writes the blob.
//
// Reads return the raw JSON string (or "null" if nothing saved yet) so the
// frontend can JSON.parse it directly.
class SettingsStore {
public:
    /// Returns the resolved settings.json path, creating parent dirs on demand.
    /// Empty path on failure (e.g. SHGetKnownFolderPath returned nothing).
    static std::filesystem::path GetPath();

    /// Reads the file's contents, or returns "null" if the file is missing.
    /// Throws on I/O error.
    static std::string Load();

    /// Atomically writes `json` (must be valid JSON; we don't re-validate).
    /// Throws on I/O error.
    static void Save(const std::string& json);
};

} // namespace SyncComms
