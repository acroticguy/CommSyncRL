#include "SyncComms/UIOverlay.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"

// BakkesMod provides ImGui context
#include "imgui.h"

namespace SyncComms {

UIOverlay::UIOverlay(std::shared_ptr<SyncState> syncState, Config* config)
    : m_syncState(std::move(syncState))
    , m_config(config)
{
    m_volume = config->GetVolume();
    m_latencyOffset = config->GetLatencyOffsetMs();
}

void UIOverlay::Render() {
    if (m_syncState->isRecording.load()) {
        RenderRecordingIndicator();
    }

    if (m_syncState->isInReplay.load()) {
        RenderPlaybackControls();
    }
}

void UIOverlay::RenderRecordingIndicator() {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(200, 60), ImGuiCond_FirstUseEver);

    ImGui::Begin("SyncComms Recording", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

    bool capturing = m_syncState->isCapturingSegment.load();
    if (capturing) {
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "[ REC ]");
        ImGui::SameLine();
        ImGui::Text("Recording segment...");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "[ ARMED ]");
        ImGui::SameLine();
        ImGui::Text("Waiting for kickoff...");
    }

    ImGui::End();
}

void UIOverlay::RenderPlaybackControls() {
    ImGui::SetNextWindowPos(ImVec2(10, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 220), ImGuiCond_FirstUseEver);

    ImGui::Begin("SyncComms Playback", nullptr, ImGuiWindowFlags_NoCollapse);

    // Segment selector
    int segCount = m_syncState->GetSegmentCount();
    int activeSeg = m_syncState->activeSegmentIndex.load();

    if (segCount > 0) {
        ImGui::Text("Segments: %d", segCount);
        ImGui::SameLine();
        if (activeSeg >= 0) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "(Active: %d)", activeSeg);
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(Between segments)");
        }
    }

    ImGui::Separator();

    // Volume control
    if (ImGui::SliderFloat("Volume", &m_volume, 0.0f, 2.0f, "%.2f")) {
        m_syncState->volumeMultiplier.store(m_volume);
    }

    // Latency offset
    if (ImGui::SliderFloat("Latency Offset (ms)", &m_latencyOffset, -500.0f, 500.0f, "%.1f")) {
        m_syncState->latencyOffsetMs.store(m_latencyOffset);
    }

    ImGui::Separator();

    // Status display
    int frame = m_syncState->currentReplayFrame.load();
    float dilation = m_syncState->timeDilation.load();
    bool paused = m_syncState->isReplayPaused.load();
    float driftMs = m_syncState->currentDriftMs.load();
    int64_t audioSample = m_syncState->currentAudioSample.load();

    ImGui::Text("Frame: %d", frame);
    ImGui::Text("Speed: %.2fx", dilation);

    if (paused) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "PAUSED");
    } else {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "PLAYING");
    }

    // Audio position in seconds
    int sampleRate = m_config->GetSampleRate();
    if (sampleRate > 0) {
        float audioSec = static_cast<float>(audioSample) / sampleRate;
        ImGui::Text("Audio: %.2fs", audioSec);
    }

    // Drift indicator
    ImVec4 driftColor;
    if (std::abs(driftMs) < 10.0f) {
        driftColor = ImVec4(0.2f, 1.0f, 0.2f, 1.0f); // green
    } else if (std::abs(driftMs) < 50.0f) {
        driftColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // yellow
    } else {
        driftColor = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // red
    }
    ImGui::TextColored(driftColor, "Drift: %.1f ms", driftMs);

    ImGui::End();
}

} // namespace SyncComms
