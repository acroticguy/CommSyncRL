#pragma once

#include "SyncComms/Config.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"

#include <memory>
#include <string>

namespace SyncComms {

/// Cvar-backed Config implementation used by the legacy BakkesMod plugin DLL.
class BakkesModConfig : public Config {
public:
    explicit BakkesModConfig(std::shared_ptr<CVarManagerWrapper> cvarManager);

    /// One-shot cvar registration; must be called once at plugin load.
    void RegisterCVars();

    bool        IsEnabled() const override;
    float       GetVolume() const override;
    float       GetLatencyOffsetMs() const override;
    std::string GetCaptureSource() const override;
    std::string GetTargetProcessName() const override;
    bool        GetIncludeMic() const override;
    std::string GetOutputDir() const override;
    int         GetSampleRate() const override;
    int         GetChannels() const override;

private:
    std::shared_ptr<CVarManagerWrapper> m_cvarManager;
};

} // namespace SyncComms
