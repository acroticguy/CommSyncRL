#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace SyncComms {

struct AudioProcessInfo {
    uint32_t    pid;
    std::string processName;    // e.g., "Discord.exe"
    std::string displayName;    // session display name if available
};

struct MicrophoneInfo {
    std::string deviceId;       // PnP-style endpoint ID — stable across reboots
    std::string friendlyName;   // user-visible, e.g. "Microphone (Razer Seiren X)"
    bool        isDefault = false;
};

class AudioSessionEnumerator {
public:
    /// Returns list of processes with active audio sessions on the default render device.
    static std::vector<AudioProcessInfo> GetActiveAudioProcesses();

    /// Find PID for a process by executable name (case-insensitive).
    static std::optional<uint32_t> FindProcessByName(const std::string& processName);

    /// Returns currently-active capture endpoints (microphones / line-ins).
    /// `isDefault` flags the system default communications device.
    static std::vector<MicrophoneInfo> GetMicrophones();
};

} // namespace SyncComms
