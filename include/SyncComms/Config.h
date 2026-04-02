#pragma once

#include <string>
#include <memory>
#include "bakkesmod/plugin/bakkesmodplugin.h"

namespace SyncComms {

class Config {
public:
    explicit Config(std::shared_ptr<CVarManagerWrapper> cvarManager);

    void RegisterCVars();

    bool IsEnabled() const;
    float GetVolume() const;
    float GetLatencyOffsetMs() const;
    std::string GetCaptureSource() const;
    std::string GetTargetProcessName() const;
    bool GetIncludeMic() const;
    std::string GetOutputDir() const;
    int GetSampleRate() const;
    int GetChannels() const;

private:
    std::shared_ptr<CVarManagerWrapper> m_cvarManager;
};

} // namespace SyncComms
