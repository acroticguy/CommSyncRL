#include "SyncComms/AudioSessionEnumerator.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <psapi.h>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "ole32.lib")

namespace SyncComms {

static std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                    nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        &result[0], size, nullptr, nullptr);
    return result;
}

static std::string GetProcessNameByPid(DWORD pid) {
    if (pid == 0) return "(System)";

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return "(unknown)";

    wchar_t path[MAX_PATH] = {};
    DWORD pathLen = MAX_PATH;
    std::string name = "(unknown)";

    if (QueryFullProcessImageNameW(hProcess, 0, path, &pathLen)) {
        std::wstring wpath(path);
        size_t pos = wpath.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            name = WideToUtf8(wpath.substr(pos + 1));
        } else {
            name = WideToUtf8(wpath);
        }
    }

    CloseHandle(hProcess);
    return name;
}

std::vector<AudioProcessInfo> AudioSessionEnumerator::GetActiveAudioProcesses() {
    std::vector<AudioProcessInfo> result;

    // Initialize COM for this call (in case it hasn't been)
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInitialized = SUCCEEDED(hrCom);

    IMMDeviceEnumerator* deviceEnum = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&deviceEnum));

    if (FAILED(hr) || !deviceEnum) {
        if (comInitialized) CoUninitialize();
        return result;
    }

    IMMDevice* device = nullptr;
    hr = deviceEnum->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        deviceEnum->Release();
        if (comInitialized) CoUninitialize();
        return result;
    }

    IAudioSessionManager2* sessionMgr = nullptr;
    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&sessionMgr));
    if (FAILED(hr) || !sessionMgr) {
        device->Release();
        deviceEnum->Release();
        if (comInitialized) CoUninitialize();
        return result;
    }

    IAudioSessionEnumerator* sessionEnum = nullptr;
    hr = sessionMgr->GetSessionEnumerator(&sessionEnum);
    if (FAILED(hr) || !sessionEnum) {
        sessionMgr->Release();
        device->Release();
        deviceEnum->Release();
        if (comInitialized) CoUninitialize();
        return result;
    }

    int sessionCount = 0;
    sessionEnum->GetCount(&sessionCount);

    DWORD myPid = GetCurrentProcessId();

    for (int i = 0; i < sessionCount; i++) {
        IAudioSessionControl* ctrl = nullptr;
        if (FAILED(sessionEnum->GetSession(i, &ctrl)) || !ctrl) continue;

        IAudioSessionControl2* ctrl2 = nullptr;
        if (FAILED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                         reinterpret_cast<void**>(&ctrl2))) || !ctrl2) {
            ctrl->Release();
            continue;
        }

        DWORD pid = 0;
        ctrl2->GetProcessId(&pid);

        // Skip system (pid 0) and our own process (Rocket League)
        if (pid != 0 && pid != myPid) {
            AudioProcessInfo info;
            info.pid = pid;
            info.processName = GetProcessNameByPid(pid);

            LPWSTR displayName = nullptr;
            if (SUCCEEDED(ctrl2->GetDisplayName(&displayName)) && displayName) {
                info.displayName = WideToUtf8(displayName);
                CoTaskMemFree(displayName);
            }

            // Avoid duplicates (same PID may have multiple sessions)
            bool duplicate = false;
            for (const auto& existing : result) {
                if (existing.pid == info.pid) { duplicate = true; break; }
            }
            if (!duplicate) {
                result.push_back(std::move(info));
            }
        }

        ctrl2->Release();
        ctrl->Release();
    }

    sessionEnum->Release();
    sessionMgr->Release();
    device->Release();
    deviceEnum->Release();
    if (comInitialized) CoUninitialize();

    return result;
}

std::optional<uint32_t> AudioSessionEnumerator::FindProcessByName(const std::string& processName) {
    if (processName.empty()) return std::nullopt;

    std::string targetLower = processName;
    std::transform(targetLower.begin(), targetLower.end(), targetLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto processes = GetActiveAudioProcesses();
    for (const auto& p : processes) {
        std::string nameLower = p.processName;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (nameLower == targetLower) {
            return p.pid;
        }
    }
    return std::nullopt;
}

} // namespace SyncComms
