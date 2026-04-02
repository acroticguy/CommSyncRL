#include "SyncComms/GuiBase.h"
#include "imgui.h"

namespace SyncComms {

// === SettingsWindowBase ===

std::string SettingsWindowBase::GetPluginName() {
    return "SyncComms - Replay Audio Sync";
}

void SettingsWindowBase::SetImGuiContext(uintptr_t ctx) {
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

// === PluginWindowBase ===

std::string PluginWindowBase::GetMenuName() {
    return "synccomms";
}

std::string PluginWindowBase::GetMenuTitle() {
    return menuTitle_;
}

void PluginWindowBase::SetImGuiContext(uintptr_t ctx) {
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

bool PluginWindowBase::ShouldBlockInput() {
    return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard;
}

bool PluginWindowBase::IsActiveOverlay() {
    return true;
}

void PluginWindowBase::OnOpen() {
    isWindowOpen_ = true;
}

void PluginWindowBase::OnClose() {
    isWindowOpen_ = false;
}

void PluginWindowBase::Render() {
    if (!isWindowOpen_) return;

    if (!ImGui::Begin(menuTitle_.c_str(), &isWindowOpen_, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }

    RenderWindow();
    ImGui::End();
}

} // namespace SyncComms
