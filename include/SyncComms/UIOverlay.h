#pragma once

#include "SyncComms/SyncState.h"
#include "SyncComms/Config.h"
#include <memory>

namespace SyncComms {

class UIOverlay {
public:
    UIOverlay(std::shared_ptr<SyncState> syncState, Config* config);

    /// Render the ImGui overlay. Called from the game thread.
    void Render();

private:
    void RenderRecordingIndicator();
    void RenderPlaybackControls();

    std::shared_ptr<SyncState> m_syncState;
    Config*                    m_config;

    // UI state
    int   m_selectedSegment = 0;
    float m_volume          = 1.0f;
    float m_latencyOffset   = 0.0f;
};

} // namespace SyncComms
