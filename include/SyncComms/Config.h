#pragma once

#include <string>

namespace SyncComms {

/// Abstract config interface consumed by the audio engine
/// (AudioCaptureManager, AudioPlaybackManager, SidecarManager).
///
/// Two concrete implementations exist:
///   - BakkesModConfig (legacy plugin DLL) — backed by cvars.
///   - StandaloneConfig (new standalone exe) — backed by SettingsStore JSON
///     plus a few static defaults populated by the orchestrator.
///
/// The engine reads via virtual methods on every access, so settings changes
/// from the UI take effect on the next call (no need to tear down the engine).
class Config {
public:
    virtual ~Config() = default;

    virtual bool        IsEnabled() const           = 0;
    virtual float       GetVolume() const           = 0;
    virtual float       GetLatencyOffsetMs() const  = 0;
    virtual std::string GetCaptureSource() const    = 0;  // "process_loopback"
    virtual std::string GetTargetProcessName() const= 0;  // "" = full system loopback
    virtual bool        GetIncludeMic() const       = 0;
    virtual std::string GetOutputDir() const        = 0;  // trailing slash expected
    virtual int         GetSampleRate() const       = 0;
    virtual int         GetChannels() const         = 0;
};

} // namespace SyncComms
