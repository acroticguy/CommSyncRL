#include "SyncComms/SyncCommsPlugin.h"
#include "SyncComms/AudioSessionEnumerator.h"
#include "bakkesmod/wrappers/ReplayServerWrapper.h"
#include "bakkesmod/wrappers/Engine/WorldInfoWrapper.h"
#include "bakkesmod/wrappers/GameEvent/ReplayDirectorWrapper.h"
#include <filesystem>
#include <fstream>
#include <chrono>

BAKKESMOD_PLUGIN(SyncComms::SyncCommsPlugin, "SyncComms", "1.0.0", PLUGINTYPE_REPLAY)

namespace SyncComms {

void SyncCommsPlugin::onLoad() {
    // 1. Initialize config
    m_config = std::make_unique<Config>(cvarManager);
    m_config->RegisterCVars();

    // 2. Create shared state
    m_syncState = std::make_shared<SyncState>();

    // 3. Create managers
    m_captureManager  = std::make_unique<AudioCaptureManager>(m_syncState, m_config.get());
    m_playbackManager = std::make_unique<AudioPlaybackManager>(m_syncState, m_config.get());
    m_sidecarManager  = std::make_unique<SidecarManager>(m_config.get(), cvarManager);
    m_uiOverlay       = std::make_unique<UIOverlay>(m_syncState, m_config.get());

    // 4. Register console commands
    cvarManager->registerNotifier("synccomms_start_capture", [this](std::vector<std::string>) {
        m_syncState->isRecording.store(true);
        // Enable auto-save replay for all game modes while recording
        cvarManager->executeCommand("ranked_autosavereplay_all 1");
        cvarManager->log("SyncComms: Recording armed — will capture on next kickoff.");
    }, "Arm audio capture for the current match", PERMISSION_ALL);

    cvarManager->registerNotifier("synccomms_stop_capture", [this](std::vector<std::string>) {
        if (m_captureManager->IsCapturing()) {
            auto seg = m_captureManager->StopCapture(GetCurrentGameFrame());
            m_capturedSegments.push_back(seg);
        }
        if (!m_capturedSegments.empty()) {
            m_sidecarManager->WriteSidecar(m_currentReplayId, m_capturedSegments);
            m_capturedSegments.clear();
        }
        m_syncState->isRecording.store(false);
        cvarManager->executeCommand("ranked_autosavereplay_all 0");
        cvarManager->log("SyncComms: Recording stopped.");
    }, "Stop audio capture", PERMISSION_ALL);

    cvarManager->registerNotifier("synccomms_refresh_audio_processes", [this](std::vector<std::string>) {
        RefreshAudioProcessList();
    }, "Refresh the audio process dropdown", PERMISSION_ALL);

    // Populate the process list on startup so the user's saved selection shows up
    RefreshAudioProcessList();

    // 5. Hook game events — capture
    gameWrapper->HookEvent(
        "Function GameEvent_Soccar_TA.Countdown.BeginState",
        [this](std::string) {
            // Auto-arm if enabled and in a valid match (not freeplay/training/replay)
            if (!m_syncState->isRecording.load() && m_config->IsEnabled() &&
                !gameWrapper->IsInReplay() && !gameWrapper->IsInFreeplay() &&
                !gameWrapper->IsInCustomTraining() &&
                (gameWrapper->IsInGame() || gameWrapper->IsInOnlineGame())) {
                m_syncState->isRecording.store(true);
                cvarManager->executeCommand("ranked_autosavereplay_all 1");
                cvarManager->log("SyncComms: Auto-armed recording for this match.");
            }

            if (m_syncState->isRecording.load()) {
                OnCountdownStarted();
            }
        });

    gameWrapper->HookEvent(
        "Function TAGame.Ball_TA.OnHitGoal",
        [this](std::string) {
            if (m_syncState->isRecording.load()) {
                OnGoalScored();
            }
        });

    gameWrapper->HookEvent(
        "Function TAGame.GameEvent_Soccar_TA.EventMatchEnded",
        [this](std::string) {
            OnMatchEnded();
        });

    // 6. Tick — handles replay detection and audio sync
    gameWrapper->HookEvent(
        "Function Engine.GameViewportClient.Tick",
        [this](std::string) {
            bool inReplay = gameWrapper->IsInReplay();

            // Detect transition into replay viewing
            if (inReplay && !m_syncState->isInReplay.load() && !m_pendingReplayLoad &&
                !m_syncState->isRecording.load()) {
                m_pendingReplayLoad = true;
                m_replayLoadRetries = 0;
            }

            // Deferred loading — try once replay data is available
            if (m_pendingReplayLoad) {
                TryLoadReplayAudio();
                return;
            }

            if (!m_syncState->isInReplay.load()) return;

            if (inReplay) {
                // Keep audio position locked to replay timeline
                auto replay = gameWrapper->GetGameEventAsReplay();
                if (!replay.IsNull()) {
                    float replayTime = replay.GetReplayTimeElapsed();

                    // Log time + active segment periodically
                    static float lastLoggedTime = -10.0f;
                    if (replayTime - lastLoggedTime > 5.0f) {
                        int activeSeg = m_syncState->activeSegmentIndex.load();
                        cvarManager->log("SyncComms: t=" + std::to_string(replayTime) +
                                         " seg=" + std::to_string(activeSeg));
                        lastLoggedTime = replayTime;
                    }

                    m_playbackManager->SyncToReplayTime(replayTime);
                }
            }
            // Don't clean up on brief FALSE blips — OnGameDestroyed handles real exits
        });

    gameWrapper->HookEvent(
        "Function TAGame.GameEvent_Soccar_TA.Destroyed",
        [this](std::string) {
            OnGameDestroyed();
        });

    // 7. Hook early-exit: player leaves before match ends
    gameWrapper->HookEvent(
        "Function TAGame.GFxData_MainMenu_TA.MainMenuAdded",
        [this](std::string) {
            FinalizeCapture();
        });

    // 8. Register Canvas overlay
    gameWrapper->RegisterDrawable(
        [this](CanvasWrapper canvas) {
            RenderOverlay(canvas);
        });

    // Reset auto-save in case it was left on from a previous session
    cvarManager->executeCommand("ranked_autosavereplay_all 0");

    cvarManager->log("SyncComms: Plugin loaded successfully.");
}

void SyncCommsPlugin::onUnload() {
    FinalizeCapture();
    gameWrapper->UnregisterDrawables();
    if (m_playbackManager) {
        m_playbackManager->StopPlayback();
    }
    m_syncState->Reset();
}

// === Capture Event Handlers ===

void SyncCommsPlugin::OnCountdownStarted() {
    if (m_captureManager->IsCapturing()) return; // already recording a segment

    if (m_currentReplayId.empty()) {
        m_currentReplayId = GetCurrentReplayId();

        // Tag the replay's display name with our ID so we can find it later
        auto server = gameWrapper->GetCurrentGameState();
        if (!server.IsNull()) {
            auto director = server.GetReplayDirector();
            if (!director.IsNull()) {
                auto replay = director.GetReplay();
                if (!replay.IsNull()) {
                    replay.SetReplayName("SyncComms_" + m_currentReplayId);
                    cvarManager->log("SyncComms: Tagged replay with ID " + m_currentReplayId);
                }
            }
        }
    }

    int startFrame = GetCurrentGameFrame();
    int segNum = static_cast<int>(m_capturedSegments.size());

    // Record the game time for accurate sync
    m_segmentStartTimeSec = 0.0;
    auto server = gameWrapper->GetCurrentGameState();
    if (!server.IsNull()) {
        m_segmentStartTimeSec = static_cast<double>(server.GetSecondsElapsed());
    }

    m_captureManager->StartCapture(m_currentReplayId, segNum, startFrame);
    cvarManager->log("SyncComms: Capture started — segment " + std::to_string(segNum) +
                     " at frame " + std::to_string(startFrame));
}

void SyncCommsPlugin::OnGoalScored() {
    if (!m_captureManager->IsCapturing()) return;

    int endFrame = GetCurrentGameFrame();
    auto seg = m_captureManager->StopCapture(endFrame);

    // Set precise timing for sync
    seg.startTimeSec = m_segmentStartTimeSec;
    seg.endTimeSec = 0.0;
    auto server = gameWrapper->GetCurrentGameState();
    if (!server.IsNull()) {
        seg.endTimeSec = static_cast<double>(server.GetSecondsElapsed());
    }

    m_capturedSegments.push_back(seg);

    cvarManager->log("SyncComms: Segment " + std::to_string(seg.index) +
                     " captured (frames " + std::to_string(seg.startFrame) +
                     "-" + std::to_string(seg.endFrame) + ")");
}

void SyncCommsPlugin::OnMatchEnded() {
    // Finalize capture if recording
    if (m_syncState->isRecording.load()) {
        if (m_captureManager->IsCapturing()) {
            int endFrame = GetCurrentGameFrame();
            auto seg = m_captureManager->StopCapture(endFrame);
            seg.startTimeSec = m_segmentStartTimeSec;
            auto server = gameWrapper->GetCurrentGameState();
            if (!server.IsNull()) {
                seg.endTimeSec = static_cast<double>(server.GetSecondsElapsed());
            }
            m_capturedSegments.push_back(seg);
        }
        if (!m_capturedSegments.empty()) {
            // Try to get the game's real replay GUID and rename files to match
            std::string gameGuid;

            // First try GetMatchGUID (works for online matches)
            auto server = gameWrapper->GetCurrentGameState();
            if (!server.IsNull()) {
                gameGuid = server.GetMatchGUID();
            }

            // Fallback: find the most recently saved replay file
            if (gameGuid.empty()) {
                gameGuid = FindReplayGuidByTag(m_currentReplayId);
            }

            if (!gameGuid.empty() && gameGuid != m_currentReplayId) {
                RenameFilesToReplayId(gameGuid);
                m_currentReplayId = gameGuid;
            }

            m_sidecarManager->WriteSidecar(m_currentReplayId, m_capturedSegments);
            cvarManager->log("SyncComms: Sidecar written with " +
                             std::to_string(m_capturedSegments.size()) + " segments for " +
                             m_currentReplayId);

            // Compress WAVs to OGG and embed in sidecar
            m_sidecarManager->CompressSegments(m_currentReplayId, m_capturedSegments);
        }
        m_capturedSegments.clear();
        m_currentReplayId.clear();
        m_syncState->isRecording.store(false);
    }

    // Stop playback if in replay
    if (m_syncState->isInReplay.load()) {
        m_playbackManager->StopPlayback();
        m_syncState->isInReplay.store(false);
    }
}

// === Playback ===

void SyncCommsPlugin::TryLoadReplayAudio() {
    m_replayLoadRetries++;
    if (m_replayLoadRetries > 300) {
        cvarManager->log("SyncComms: Gave up loading after 300 retries.");
        m_pendingReplayLoad = false;
        return;
    }

    auto replayServer = gameWrapper->GetGameEventAsReplay();
    if (replayServer.IsNull()) {
        if (m_replayLoadRetries == 1) cvarManager->log("SyncComms: TryLoad — replayServer null");
        return;
    }

    // Get the replay object directly from the ReplayServerWrapper
    auto replay = replayServer.GetReplay();
    if (replay.IsNull()) {
        if (m_replayLoadRetries == 1) cvarManager->log("SyncComms: TryLoad — replay null");
        return;
    }

    // Read all available IDs
    std::string matchGuid = replayServer.GetMatchGUID();

    // Skip if we already loaded this replay
    if (!matchGuid.empty() && matchGuid == m_loadedReplayGuid) {
        m_pendingReplayLoad = false;
        return;
    }
    std::string replayName = replay.GetReplayName().ToString();
    std::string replayId = replay.GetId().ToString();

    cvarManager->log("SyncComms: Replay name='" + replayName +
                     "' id='" + replayId +
                     "' matchGuid='" + matchGuid + "'");

    // Try to find a sidecar using multiple strategies
    std::optional<std::string> sidecarPath;

    // 1. Try our tagged name
    const std::string prefix = "SyncComms_";
    if (replayName.size() > prefix.size() &&
        replayName.substr(0, prefix.size()) == prefix) {
        std::string tagId = replayName.substr(prefix.size());
        sidecarPath = m_sidecarManager->FindSidecar(tagId);
        if (sidecarPath.has_value()) {
            cvarManager->log("SyncComms: Matched by tagged name: " + tagId);
        }
    }

    // 2. Try match GUID
    if (!sidecarPath.has_value() && !matchGuid.empty() &&
        matchGuid.find("No ") == std::string::npos) {
        sidecarPath = m_sidecarManager->FindSidecar(matchGuid);
        if (sidecarPath.has_value()) {
            cvarManager->log("SyncComms: Matched by match GUID: " + matchGuid);
        }
    }

    // 3. Try replay ID
    if (!sidecarPath.has_value() && !replayId.empty()) {
        sidecarPath = m_sidecarManager->FindSidecar(replayId);
    }

    // 4. Last resort fallback
    if (!sidecarPath.has_value()) {
        sidecarPath = m_sidecarManager->FindSidecarByFrame(0);
    }

    if (!sidecarPath.has_value()) {
        cvarManager->log("SyncComms: No sidecar found for this replay.");
        m_pendingReplayLoad = false; // stop trying
        return;
    }

    auto segments = m_sidecarManager->ReadSidecar(sidecarPath.value());
    if (segments.empty()) {
        m_pendingReplayLoad = false;
        return;
    }

    m_syncState->SetSegments(segments);
    m_syncState->isInReplay.store(true);
    m_pendingReplayLoad = false;
    m_loadedReplayGuid = matchGuid;

    m_playbackManager->LoadSegments(segments);
    cvarManager->log("SyncComms: Loaded " + std::to_string(segments.size()) +
                     " segments from " +
                     std::filesystem::path(sidecarPath.value()).filename().string());
    for (const auto& seg : segments) {
        cvarManager->log("  seg" + std::to_string(seg.index) +
                         " t=" + std::to_string(seg.startTimeSec) +
                         "-" + std::to_string(seg.endTimeSec) +
                         " file=" + seg.audioFile);
    }
}

void SyncCommsPlugin::OnGameDestroyed() {
    FinalizeCapture();

    // Stop playback
    if (m_playbackManager) {
        m_playbackManager->StopPlayback();
    }
    m_syncState->isInReplay.store(false);
    m_pendingReplayLoad = false;
    m_loadedReplayGuid.clear();
}

// === Graceful Early Exit ===

void SyncCommsPlugin::FinalizeCapture() {
    if (!m_syncState->isRecording.load()) return;

    // Close any active audio segment
    if (m_captureManager && m_captureManager->IsCapturing()) {
        int endFrame = GetCurrentGameFrame();
        auto seg = m_captureManager->StopCapture(endFrame);
        seg.startTimeSec = m_segmentStartTimeSec;
        auto server = gameWrapper->GetCurrentGameState();
        if (!server.IsNull()) {
            seg.endTimeSec = static_cast<double>(server.GetSecondsElapsed());
        }
        m_capturedSegments.push_back(seg);
        cvarManager->log("SyncComms: Finalized active segment at frame " + std::to_string(endFrame));
    }

    // Write sidecar with everything captured so far
    if (!m_capturedSegments.empty()) {
        m_sidecarManager->WriteSidecar(m_currentReplayId, m_capturedSegments);
        m_sidecarManager->CompressSegments(m_currentReplayId, m_capturedSegments);
        cvarManager->log("SyncComms: Saved " + std::to_string(m_capturedSegments.size()) +
                         " segments.");
        m_capturedSegments.clear();
    }

    m_currentReplayId.clear();
    m_syncState->isRecording.store(false);
    cvarManager->executeCommand("ranked_autosavereplay_all 0");
}

void SyncCommsPlugin::RenameFilesToReplayId(const std::string& newId) {
    std::string outputDir = m_config->GetOutputDir();

    // Build the new short ID the same way BuildFilePath does
    std::string newShortId = newId;
    for (const auto& prefix : {"synccomms_", "sc_"}) {
        if (newShortId.size() > strlen(prefix) &&
            newShortId.substr(0, strlen(prefix)) == prefix) {
            newShortId = newShortId.substr(strlen(prefix));
            break;
        }
    }

    for (auto& seg : m_capturedSegments) {
        std::string oldPath = outputDir + seg.audioFile;

        // Build new filename with same pattern: sc_<newShortId>_segNNN.wav
        char buf[128];
        snprintf(buf, sizeof(buf), "sc_%s_seg%03d.wav", newShortId.c_str(), seg.index);
        std::string newFilename(buf);
        std::string newPath = outputDir + newFilename;

        std::error_code ec;
        std::filesystem::rename(oldPath, newPath, ec);
        if (!ec) {
            seg.audioFile = newFilename;
            cvarManager->log("SyncComms: Renamed " + seg.audioFile + " -> " + newFilename);
        } else {
            cvarManager->log("SyncComms: Failed to rename " + oldPath + ": " + ec.message());
        }
    }

    // Also remove the old sidecar if it exists
    std::string oldSidecar = outputDir + m_currentReplayId + "_synccomms.json";
    std::filesystem::remove(oldSidecar);
}

std::string SyncCommsPlugin::FindReplayGuidByTag(const std::string& tag) {
    std::string userProfile = std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "";
    std::string demosDir = userProfile + "/Documents/My Games/Rocket League/TAGame/Demos/";

    if (!std::filesystem::exists(demosDir)) return "";

    // Find the most recently modified .replay file
    std::string newestFile;
    std::filesystem::file_time_type newestTime{};

    for (const auto& entry : std::filesystem::directory_iterator(demosDir)) {
        if (entry.path().extension() != ".replay") continue;
        auto t = entry.last_write_time();
        if (newestFile.empty() || t > newestTime) {
            newestTime = t;
            newestFile = entry.path().string();
        }
    }

    if (newestFile.empty()) return "";

    // Check if this file was modified in the last 30 seconds (i.e., just saved)
    auto now = std::filesystem::file_time_type::clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - newestTime).count();
    if (age > 30) return "";

    return std::filesystem::path(newestFile).stem().string();
}

// === Canvas Overlay ===

void SyncCommsPlugin::RenderOverlay(CanvasWrapper canvas) {
    if (!m_config || !m_config->IsEnabled()) return;

    bool recording = m_syncState->isRecording.load();
    bool capturing = m_syncState->isCapturingSegment.load();
    bool inReplay  = m_syncState->isInReplay.load();

    // Only show overlay when something is happening
    if (!recording && !inReplay) return;

    float x = 10.0f;
    float y = 10.0f;

    // Background
    canvas.SetColor(0, 0, 0, 160);
    canvas.SetPosition(Vector2F{x, y});
    canvas.FillBox(Vector2F{220.0f, recording ? 70.0f : 40.0f});

    // Status line
    canvas.SetPosition(Vector2F{x + 8.0f, y + 6.0f});
    if (recording) {
        if (capturing) {
            canvas.SetColor(255, 50, 50, 255);
            canvas.DrawString("[ REC ] Recording...", 1.2f, 1.2f);
        } else {
            canvas.SetColor(255, 200, 0, 255);
            canvas.DrawString("[ ARMED ] Waiting...", 1.2f, 1.2f);
        }

        // Segment count
        int segCount = static_cast<int>(m_capturedSegments.size());
        canvas.SetPosition(Vector2F{x + 8.0f, y + 28.0f});
        canvas.SetColor(200, 200, 200, 255);
        canvas.DrawString("Clips: " + std::to_string(segCount), 1.0f, 1.0f);

        // Currently capturing indicator
        if (capturing) {
            canvas.SetPosition(Vector2F{x + 8.0f, y + 48.0f});
            canvas.SetColor(255, 100, 100, 255);
            canvas.DrawString("Frame: " + std::to_string(GetCurrentGameFrame()), 1.0f, 1.0f);
        }
    } else if (inReplay) {
        int segCount = m_syncState->GetSegmentCount();
        int activeSeg = m_syncState->activeSegmentIndex.load();
        canvas.SetColor(100, 200, 255, 255);
        std::string text = "Playback";
        if (activeSeg >= 0) {
            text += " [" + std::to_string(activeSeg + 1) + "/" + std::to_string(segCount) + "]";
        }
        canvas.DrawString(text, 1.2f, 1.2f);
    }
}

// === Helpers ===

std::string SyncCommsPlugin::GetCurrentReplayId() {
    // When viewing a replay, try to identify it from the replay data
    if (gameWrapper->IsInReplay()) {
        auto replayServer = gameWrapper->GetGameEventAsReplay();
        if (!replayServer.IsNull()) {
            auto director = replayServer.GetReplayDirector();
            if (!director.IsNull()) {
                auto replay = director.GetReplay();
                if (!replay.IsNull()) {
                    // Log everything so we can see what's available
                    std::string name = replay.GetReplayName().ToString();
                    std::string id = replay.GetId().ToString();
                    std::string matchGuid = replayServer.GetMatchGUID();
                    cvarManager->log("SyncComms: Replay name='" + name +
                                     "' id='" + id +
                                     "' matchGuid='" + matchGuid + "'");

                    // 1. Try our tagged name first
                    const std::string prefix = "SyncComms_";
                    if (name.size() > prefix.size() &&
                        name.substr(0, prefix.size()) == prefix) {
                        return name.substr(prefix.size());
                    }

                    // 2. Try match GUID (filter garbage values)
                    if (!matchGuid.empty() && matchGuid.find("No ") == std::string::npos) {
                        return matchGuid;
                    }

                    // 3. Try replay's own ID
                    if (!id.empty()) return id;
                }
            }
        }
    }

    // During live match, get match GUID from the game server
    auto server = gameWrapper->GetGameEventAsServer();
    if (!server.IsNull()) {
        std::string guid = server.GetMatchGUID();
        if (!guid.empty()) return guid;
    }

    // For offline/exhibition: try to get the replay's own ID
    auto gameState = gameWrapper->GetCurrentGameState();
    if (!gameState.IsNull()) {
        auto director = gameState.GetReplayDirector();
        if (!director.IsNull()) {
            auto replay = director.GetReplay();
            if (!replay.IsNull()) {
                std::string replayId = replay.GetId().ToString();
                if (!replayId.empty()) return replayId;
            }
        }
    }

    // Last resort: timestamp-based ID
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
    return "sc_" + std::to_string(ms);
}

int SyncCommsPlugin::GetCurrentGameFrame() {
    if (gameWrapper->IsInReplay()) {
        auto replay = gameWrapper->GetGameEventAsReplay();
        if (!replay.IsNull()) {
            return replay.GetCurrentReplayFrame();
        }
    }
    // During live match, use the internal frame counter
    // This approximation uses game time * expected frame rate
    auto server = gameWrapper->GetCurrentGameState();
    if (!server.IsNull()) {
        float gameTime = server.GetSecondsElapsed();
        return static_cast<int>(gameTime * 30.0f); // ~30 fps replay rate
    }
    return 0;
}

void SyncCommsPlugin::RefreshAudioProcessList() {
    auto processes = AudioSessionEnumerator::GetActiveAudioProcesses();

    // Build combobox options: "System Audio (All)@&Discord.exe@Discord.exe&..."
    std::string options = "System Audio (All)@";
    for (const auto& p : processes) {
        options += "&" + p.processName + "@" + p.processName;
    }

    // Regenerate the .set file with the populated dropdown
    std::string setPath = std::string(std::getenv("APPDATA"))
        + "/bakkesmod/bakkesmod/plugins/settings/synccomms.set";

    std::ofstream file(setPath);
    if (file.is_open()) {
        file << "SyncComms - Replay Audio Sync\n";
        file << "1|Enable SyncComms|synccomms_enabled\n";
        file << "9|Audio Source\n";
        file << "6|Target Process|synccomms_target_process|" << options << "\n";
        file << "0|Refresh Process List|synccomms_refresh_audio_processes\n";
        file << "1|Include Microphone|synccomms_include_mic\n";
        file << "9|Playback Settings\n";
        file << "4|Volume|synccomms_volume|0|2\n";
        file << "3|Latency Offset (ms)|synccomms_latency_offset_ms|-500|500\n";
        file << "9|Advanced\n";
        file << "5|Sample Rate|synccomms_sample_rate|8000|96000\n";
        file << "5|Channels|synccomms_channels|1|2\n";
        file.close();

        cvarManager->executeCommand("cl_settings_refreshplugins");
    }
}

} // namespace SyncComms
