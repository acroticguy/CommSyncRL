#include "SyncComms/StandaloneConfig.h"

#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <filesystem>

#pragma comment(lib, "shell32.lib")

namespace SyncComms {

namespace {

std::string DefaultOutputDir() {
    PWSTR raw = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw);
    if (FAILED(hr) || !raw) {
        if (raw) CoTaskMemFree(raw);
        return {};
    }
    std::filesystem::path p(raw);
    CoTaskMemFree(raw);
    p /= L"SyncComms";
    p /= L"recordings";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    auto s = p.u8string();
    if (!s.empty() && s.back() != '/' && s.back() != '\\') s.push_back('/');
    return s;
}

} // namespace

StandaloneConfig::StandaloneConfig() {
    SetOutputDir(DefaultOutputDir());
}

bool        StandaloneConfig::IsEnabled() const          { return m_enabled.load(); }
float       StandaloneConfig::GetVolume() const          { return m_volume.load(); }
float       StandaloneConfig::GetLatencyOffsetMs() const { return m_latencyOffsetMs.load(); }
std::string StandaloneConfig::GetCaptureSource() const   { return "process_loopback"; }
bool        StandaloneConfig::GetIncludeMic() const      { return m_includeMic.load(); }
int         StandaloneConfig::GetSampleRate() const      { return m_sampleRate.load(); }
int         StandaloneConfig::GetChannels() const        { return m_channels.load(); }

std::string StandaloneConfig::GetTargetProcessName() const {
    std::lock_guard<std::mutex> lock(m_strMutex);
    return m_targetProcess;
}
std::string StandaloneConfig::GetOutputDir() const {
    std::lock_guard<std::mutex> lock(m_strMutex);
    return m_outputDir;
}

} // namespace SyncComms
