#pragma once

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/wrappers/canvaswrapper.h"
#include "SyncComms/SyncState.h"
#include "SyncComms/Config.h"
#include "SyncComms/AudioCaptureManager.h"
#include "SyncComms/AudioPlaybackManager.h"
#include "SyncComms/SidecarManager.h"
#include "SyncComms/UIOverlay.h"
#include <memory>
#include <vector>
#include <string>

namespace SyncComms {

class SyncCommsPlugin : public BakkesMod::Plugin::BakkesModPlugin {
public:
    void onLoad() override;
    void onUnload() override;

private:
    // Event handlers — Capture phase
    void OnCountdownStarted();
    void OnGoalScored();
    void OnMatchEnded();

    // Playback
    void TryLoadReplayAudio();
    void OnGameDestroyed();

    // Canvas overlay
    void RenderOverlay(CanvasWrapper canvas);

    // Early exit — safely finalize capture
    void FinalizeCapture();

    // Rename audio files and update segments to use the real replay ID
    void RenameFilesToReplayId(const std::string& newId);

    // Find the replay file GUID by scanning recently saved replays
    std::string FindReplayGuidByTag(const std::string& tag);

    // Helpers
    std::string GetCurrentReplayId();
    int GetCurrentGameFrame();
    void RefreshAudioProcessList();

    // Owned components
    std::shared_ptr<SyncState>                  m_syncState;
    std::unique_ptr<Config>                     m_config;
    std::unique_ptr<AudioCaptureManager>        m_captureManager;
    std::unique_ptr<AudioPlaybackManager>       m_playbackManager;
    std::unique_ptr<SidecarManager>             m_sidecarManager;
    std::unique_ptr<UIOverlay>                  m_uiOverlay;

    // Capture session state
    std::string              m_currentReplayId;
    std::vector<SegmentInfo> m_capturedSegments;
    double                   m_segmentStartTimeSec = 0.0;
    bool                     m_pendingReplayLoad   = false;
    int                      m_replayLoadRetries   = 0;
    std::string              m_loadedReplayGuid;  // prevent re-loading same replay
};

} // namespace SyncComms
