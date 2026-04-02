#include "SyncComms/Config.h"
#include <cstdlib>

namespace SyncComms {

Config::Config(std::shared_ptr<CVarManagerWrapper> cvarManager)
    : m_cvarManager(std::move(cvarManager))
{
}

void Config::RegisterCVars() {
    m_cvarManager->registerCvar("synccomms_enabled", "1", "Enable SyncComms plugin");
    m_cvarManager->registerCvar("synccomms_volume", "1.0", "Playback volume");
    m_cvarManager->registerCvar("synccomms_latency_offset_ms", "0", "Latency offset in ms");
    m_cvarManager->registerCvar("synccomms_target_process", "", "Target process for audio capture (empty = full system loopback)");
    m_cvarManager->registerCvar("synccomms_include_mic", "1", "Include microphone input alongside application audio");
    m_cvarManager->registerCvar("synccomms_sample_rate", "48000", "Audio sample rate");
    m_cvarManager->registerCvar("synccomms_channels", "1", "Audio channels (1=mono, 2=stereo)");

    // Build default output directory
    std::string appdata = std::getenv("APPDATA") ? std::getenv("APPDATA") : "";
    std::string defaultDir = appdata + "/bakkesmod/bakkesmod/data/synccomms/";
    m_cvarManager->registerCvar("synccomms_output_dir", defaultDir, "Output directory for audio files");
}

bool Config::IsEnabled() const {
    auto cvar = m_cvarManager->getCvar("synccomms_enabled");
    return cvar ? cvar.getBoolValue() : true;
}

float Config::GetVolume() const {
    auto cvar = m_cvarManager->getCvar("synccomms_volume");
    return cvar ? cvar.getFloatValue() : 1.0f;
}

float Config::GetLatencyOffsetMs() const {
    auto cvar = m_cvarManager->getCvar("synccomms_latency_offset_ms");
    return cvar ? cvar.getFloatValue() : 0.0f;
}

std::string Config::GetCaptureSource() const {
    return "process_loopback";
}

std::string Config::GetTargetProcessName() const {
    auto cvar = m_cvarManager->getCvar("synccomms_target_process");
    return cvar ? cvar.getStringValue() : "";
}

bool Config::GetIncludeMic() const {
    auto cvar = m_cvarManager->getCvar("synccomms_include_mic");
    return cvar ? cvar.getBoolValue() : true;
}

std::string Config::GetOutputDir() const {
    auto cvar = m_cvarManager->getCvar("synccomms_output_dir");
    return cvar ? cvar.getStringValue() : "";
}

int Config::GetSampleRate() const {
    auto cvar = m_cvarManager->getCvar("synccomms_sample_rate");
    return cvar ? cvar.getIntValue() : 48000;
}

int Config::GetChannels() const {
    auto cvar = m_cvarManager->getCvar("synccomms_channels");
    return cvar ? cvar.getIntValue() : 1;
}

} // namespace SyncComms
