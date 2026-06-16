// SyncCommsApp.exe — standalone shell.
//
// Hosts WebView2 and bridges native C++ functions into a Svelte frontend.
// At launch, decides where the UI comes from:
//   1. If SYNCCOMMS_DEV_URL is set in the environment   → navigate to that URL
//      (typical dev workflow: `npm run dev` serves on http://localhost:5173,
//       which lets us use Vite's HMR while iterating on Svelte components)
//   2. Else if frontend/index.html exists next to exe   → read it and pass to
//      WebView2's NavigateToString. Vite's singlefile build inlines all JS
//      and CSS, so there are no external resource loads from the document and
//      we sidestep the file:// → null-origin → CORS-blocked-modules problem
//      that breaks plain navigate("file://...") for any non-trivial Vite app.
//   3. Else                                              → show inline fallback
//      error page so the user gets a useful message instead of a blank window.

#include <webview/webview.h>
#include <nlohmann/json.hpp>

#include "SyncComms/AudioSessionEnumerator.h"
#include "SyncComms/SettingsStore.h"
#include "SyncComms/StatsApiClient.h"
#include "SyncComms/StandaloneConfig.h"
#include "SyncComms/CaptureOrchestrator.h"
#include "SyncComms/PlaybackController.h"
#include "SyncComms/RecordingMaintenance.h"
#include "SyncComms/ReplayLocator.h"

#include <windows.h>
#include <objbase.h>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

constexpr const char* kFallbackHtml = R"HTML(
<!doctype html>
<html><head><meta charset="utf-8"><title>SyncComms</title>
<style>
  body { font: 14px system-ui, sans-serif; background: #0e1014; color: #e6e8ee;
         margin: 0; padding: 40px; }
  h1 { font-size: 16px; margin: 0 0 12px; }
  p { color: #8a93a6; max-width: 520px; line-height: 1.55; }
  code { background: #1c2030; padding: 1px 6px; border-radius: 3px; font-size: 12.5px; }
</style></head><body>
  <h1>SyncComms — frontend not found</h1>
  <p>No frontend resources were found next to this executable, and
     <code>SYNCCOMMS_DEV_URL</code> is not set.</p>
  <p>To run in dev mode: <code>cd frontend && npm run dev</code> in one
     terminal, then launch this exe with
     <code>SYNCCOMMS_DEV_URL=http://localhost:5173</code> set.</p>
  <p>To run a release build: ensure <code>frontend/index.html</code> sits next
     to <code>SyncCommsApp.exe</code> (CMake copies it from
     <code>frontend/dist/</code> after <code>npm run build</code>).</p>
</body></html>
)HTML";

std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return L"";
    std::filesystem::path p(buf);
    return p.parent_path().wstring();
}

// This is a /SUBSYSTEM:WINDOWS app, so std::cerr is attached to nothing when
// launched normally (double-click or from a GUI). All the timing diagnostics
// (CaptureOrchestrator's GoalScored/segment lines, PlaybackController's
// [Anchor] / [playback] tick, PostMatchLink) write to std::cerr — redirect it
// to a log file so they're observable. Works for both double-click and
// terminal launches; tail it live with `Get-Content -Wait`. The ofstream is
// intentionally a leaked static so its buffer outlives every cerr write
// through shutdown. Unbuffered (unitbuf) so a crash still leaves the last
// lines on disk.
void InitDiagnosticLog() {
    namespace fs = std::filesystem;
    fs::path dir;
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) {
        dir = fs::path(appdata) / "SyncComms";
    } else {
        dir = fs::path(GetExeDir());
    }
    std::error_code ec;
    fs::create_directories(dir, ec);

    static std::ofstream logFile(dir / "synccomms.log",
                                 std::ios::out | std::ios::trunc);
    if (logFile.is_open()) {
        std::cerr.rdbuf(logFile.rdbuf());
        std::cerr << std::unitbuf
                  << "[main] SyncComms log started — "
                  << (dir / "synccomms.log").u8string() << "\n";
    }
}

std::string ReadEnv(const char* name) {
    char buf[2048];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return "";
    return std::string(buf, n);
}

struct FrontendSource {
    enum class Kind { DevUrl, InlineHtml, NotFound };
    Kind kind = Kind::NotFound;
    std::string payload; // URL for DevUrl, full HTML for InlineHtml, empty otherwise
};

FrontendSource ResolveFrontend() {
    FrontendSource fs;

    auto devUrl = ReadEnv("SYNCCOMMS_DEV_URL");
    if (!devUrl.empty()) {
        fs.kind = FrontendSource::Kind::DevUrl;
        fs.payload = devUrl;
        return fs;
    }

    std::filesystem::path indexPath = std::filesystem::path(GetExeDir()) /
                                       L"frontend" / L"index.html";
    if (std::filesystem::exists(indexPath)) {
        std::ifstream in(indexPath, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            fs.kind = FrontendSource::Kind::InlineHtml;
            fs.payload = ss.str();
            return fs;
        }
    }

    return fs;
}

std::string ListAudioProcessesAsJson() {
    auto procs = SyncComms::AudioSessionEnumerator::GetActiveAudioProcesses();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : procs) {
        arr.push_back({
            {"pid", p.pid},
            {"processName", p.processName},
            {"displayName", p.displayName},
        });
    }
    return arr.dump();
}

std::string ListMicrophonesAsJson() {
    auto mics = SyncComms::AudioSessionEnumerator::GetMicrophones();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : mics) {
        arr.push_back({
            {"deviceId", m.deviceId},
            {"friendlyName", m.friendlyName},
            {"isDefault", m.isDefault},
        });
    }
    return arr.dump();
}

// Inspect one sidecar in detail: per-segment timing fields plus audioData
// length (NOT the data itself — base64 OGGs are huge and we just need the
// length for diagnostics). Used from the F12 console to diagnose playback
// issues like "is the audio data actually populated".
std::string InspectRecordingAsJson(const std::string& outputDir,
                                   const std::string& matchGuid) {
    nlohmann::json out;
    if (outputDir.empty() || matchGuid.empty()) {
        out["error"] = "missing arguments";
        return out.dump();
    }
    std::filesystem::path path =
        std::filesystem::path(outputDir) / (matchGuid + "_synccomms.json");
    out["path"] = path.u8string();
    if (!std::filesystem::exists(path)) {
        out["error"] = "file not found";
        return out.dump();
    }
    nlohmann::json side;
    try {
        std::ifstream in(path, std::ios::binary);
        in >> side;
    } catch (const std::exception& e) {
        out["error"] = std::string("parse failed: ") + e.what();
        return out.dump();
    }

    out["replayId"]  = side.value("replayId", std::string{});
    out["version"]   = side.value("version", 0);
    out["frameTime"] = side.value("frameTime", 0.0);

    nlohmann::json segs = nlohmann::json::array();
    if (side.contains("segments") && side["segments"].is_array()) {
        for (auto& s : side["segments"]) {
            const std::string ad = s.value("audioData", std::string{});
            segs.push_back({
                {"index",         s.value("index", 0)},
                {"startFrame",    s.value("startFrame", 0)},
                {"endFrame",      s.value("endFrame", 0)},
                {"startTimeSec",  s.value("startTimeSec", 0.0)},
                {"endTimeSec",    s.value("endTimeSec", 0.0)},
                {"audioFile",     s.value("audioFile", std::string{})},
                {"audioDataLen",  static_cast<int64_t>(ad.size())},
                {"event",         s.value("event", std::string{})},
            });
        }
    }
    out["segments"] = segs;
    return out.dump();
}

// Walk the recordings folder, build a lightweight summary for each sidecar
// (no audio blobs). Frontend renders this in the Recordings view.
std::string ListRecordingsAsJson(const std::string& outputDir) {
    nlohmann::json arr = nlohmann::json::array();
    if (outputDir.empty()) return arr.dump();

    std::error_code ec;
    if (!std::filesystem::exists(outputDir, ec)) return arr.dump();

    for (const auto& entry : std::filesystem::directory_iterator(outputDir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        // Sidecars live as <matchGuid>_synccomms.json (per SidecarManager).
        if (path.extension() != ".json") continue;
        if (path.stem().u8string().find("_synccomms") == std::string::npos) continue;

        nlohmann::json side;
        try {
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;
            in >> side;
        } catch (...) {
            continue;
        }

        std::string matchGuid = side.value("replayId", std::string{});
        double totalDur = 0.0;
        int segCount = 0;
        if (side.contains("segments") && side["segments"].is_array()) {
            for (auto& s : side["segments"]) {
                ++segCount;
                double a = s.value("startTimeSec", 0.0);
                double b = s.value("endTimeSec", 0.0);
                if (b > a) totalDur += (b - a);
            }
        }

        // Modification time as ISO-ish string (UTC, second precision).
        auto ftime = std::filesystem::last_write_time(path, ec);
        std::time_t mtime = 0;
        if (!ec) {
            // Convert filesystem time to system_clock; portable form.
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            mtime = std::chrono::system_clock::to_time_t(sctp);
        }

        arr.push_back({
            {"matchGuid", matchGuid},
            {"filePath",  path.u8string()},
            {"fileSize",  static_cast<int64_t>(std::filesystem::file_size(path, ec))},
            {"modifiedAtUnix", static_cast<int64_t>(mtime)},
            {"segmentCount", segCount},
            {"totalDurationSec", totalDur},
        });
    }

    // Newest first.
    std::sort(arr.begin(), arr.end(), [](const auto& a, const auto& b) {
        return a.value("modifiedAtUnix", int64_t{0}) > b.value("modifiedAtUnix", int64_t{0});
    });
    return arr.dump();
}

// Push every field from the persisted-settings JSON into the live
// StandaloneConfig. Called once at startup (after loadSettings) and again on
// every saveSettings — keeps the audio engine reading whatever the UI chose.
void ApplySettingsJson(const std::string& json, SyncComms::StandaloneConfig& cfg) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json);
    } catch (...) {
        return;
    }
    if (!j.is_object()) return;

    if (j.contains("recordingEnabled") && j["recordingEnabled"].is_boolean()) {
        cfg.SetEnabled(j["recordingEnabled"].get<bool>());
    }
    if (j.contains("captureTarget")) {
        const auto& v = j["captureTarget"];
        cfg.SetTargetProcessName(v.is_string() ? v.get<std::string>() : "");
    }
    if (j.contains("includeMic") && j["includeMic"].is_boolean()) {
        cfg.SetIncludeMic(j["includeMic"].get<bool>());
    }
    if (j.contains("microphoneId")) {
        const auto& v = j["microphoneId"];
        cfg.SetMicrophoneId(v.is_string() ? v.get<std::string>() : "");
    }
    if (j.contains("volume") && j["volume"].is_number()) {
        cfg.SetVolume(j["volume"].get<float>());
    }
    if (j.contains("latencyOffsetMs") && j["latencyOffsetMs"].is_number()) {
        cfg.SetLatencyOffsetMs(j["latencyOffsetMs"].get<float>());
    }
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // WebView2 runs each call to its callbacks on the UI thread, which the
    // webview library pumps for us. The audio session enumerator inits COM
    // itself per call, so we don't need a process-wide CoInitialize here.
    try {
        InitDiagnosticLog();

        // `debug = true` enables Edge DevTools (F12). Leaving on for now while
        // we're iterating on the UI; flip off for shipped builds.
        webview::webview w(/* debug = */ true, /* window = */ nullptr);
        w.set_title("SyncComms");
        w.set_size(960, 640, WEBVIEW_HINT_NONE);

        w.bind("listAudioProcesses",
               [](const std::string& /* args_json */) -> std::string {
                   return ListAudioProcessesAsJson();
               });

        w.bind("listMicrophones",
               [](const std::string& /* args_json */) -> std::string {
                   return ListMicrophonesAsJson();
               });

        // Live config used by the audio engine. Hydrated from settings.json
        // at startup, refreshed on every saveSettings call from the UI.
        SyncComms::StandaloneConfig liveConfig;
        ApplySettingsJson(SyncComms::SettingsStore::Load(), liveConfig);

        w.bind("listRecordings",
               [&liveConfig](const std::string& /* args_json */) -> std::string {
                   return ListRecordingsAsJson(liveConfig.GetOutputDir());
               });

        w.bind("inspectRecording",
               [&liveConfig](const std::string& args_json) -> std::string {
                   auto args = nlohmann::json::parse(args_json);
                   std::string guid;
                   if (args.is_array() && !args.empty() && args[0].is_string()) {
                       guid = args[0].get<std::string>();
                   }
                   return InspectRecordingAsJson(liveConfig.GetOutputDir(), guid);
               });

        // Delete one recording (sidecar + any WAVs it still references) by its
        // replay id. Returns "true"/"false"; the frontend refreshes its list.
        w.bind("deleteRecording",
               [&liveConfig](const std::string& args_json) -> std::string {
                   auto args = nlohmann::json::parse(args_json);
                   std::string guid;
                   if (args.is_array() && !args.empty() && args[0].is_string()) {
                       guid = args[0].get<std::string>();
                   }
                   bool ok = SyncComms::DeleteRecording(
                       liveConfig.GetOutputDir(), guid,
                       [](const std::string& m) { std::cerr << m << "\n"; });
                   return ok ? "true" : "false";
               });

        // Settings persistence. The frontend reads this once on startup (via
        // the shared store) and writes the full settings blob whenever
        // anything changes. Schema is owned by the frontend (lib/types.ts).
        w.bind("loadSettings",
               [](const std::string& /* args_json */) -> std::string {
                   return SyncComms::SettingsStore::Load();
               });

        w.bind("saveSettings",
               [&liveConfig](const std::string& args_json) -> std::string {
                   // webview encodes JS args as a JSON array; saveSettings(obj)
                   // arrives as `[obj]`. We write `obj` (pretty-printed) AND
                   // push it into the in-memory config so the audio engine
                   // sees the change immediately.
                   auto args = nlohmann::json::parse(args_json);
                   if (!args.is_array() || args.empty()) {
                       throw std::runtime_error("saveSettings: expected one argument");
                   }
                   const std::string blob = args[0].dump(2);
                   SyncComms::SettingsStore::Save(blob);
                   ApplySettingsJson(blob, liveConfig);
                   return "true";
               });

        // Stats API client: connects to RL on a background thread and pushes
        // every parsed event into the frontend through `window.__onRlEvent`.
        // Connection state changes go through `window.__onRlState`. Both are
        // resilient to the frontend not having registered the receivers yet
        // (the JS side guards with `typeof === "function"`).
        SyncComms::StatsApiClient stats;

        auto pushToJs = [&w](const std::string& js) {
            // dispatch() schedules the call on the WebView2 UI thread.
            w.dispatch([&w, js]() {
                try {
                    w.eval(js);
                } catch (...) {
                    // Frontend not ready yet, or webview is shutting down.
                }
            });
        };

        // Capture orchestrator: turns Stats API events into audio capture
        // lifecycle calls and writes a sidecar at MatchEnded.
        SyncComms::CaptureOrchestrator orchestrator(liveConfig);

        // Playback controller: drives audio playback synced to the in-game
        // replay viewer's Elapsed/Frame.
        SyncComms::PlaybackController playback(liveConfig);

        auto pushPlaybackStatus = [&pushToJs](const SyncComms::PlaybackStatus& s) {
            const char* phase = "idle";
            switch (s.phase) {
                case SyncComms::PlaybackPhase::Idle:             phase = "idle"; break;
                case SyncComms::PlaybackPhase::SearchingSidecar: phase = "searching"; break;
                case SyncComms::PlaybackPhase::NoSidecar:        phase = "noSidecar"; break;
                case SyncComms::PlaybackPhase::Playing:          phase = "playing"; break;
                case SyncComms::PlaybackPhase::Error:            phase = "error"; break;
            }
            nlohmann::json payload = {
                {"phase", phase},
                {"matchGuid", s.matchGuid},
                {"sidecarPath", s.sidecarPath},
                {"replayPath", s.replayPath},
                {"segmentCount", s.segmentCount},
                {"activeSegmentIndex", s.activeSegmentIndex},
                {"elapsedSec", s.elapsedSec},
                {"anchored", s.anchored},
                {"lastError", s.lastError},
            };
            pushToJs(
                "if (typeof window.__onPlaybackStatus === 'function') "
                "window.__onPlaybackStatus(" + payload.dump() + ");"
            );
        };
        playback.SetStatusCallback(pushPlaybackStatus);

        auto pushCaptureStatus = [&pushToJs](const SyncComms::CaptureStatus& s) {
            const char* phase = "idle";
            switch (s.phase) {
                case SyncComms::CapturePhase::Idle:       phase = "idle"; break;
                case SyncComms::CapturePhase::Armed:      phase = "armed"; break;
                case SyncComms::CapturePhase::Recording:  phase = "recording"; break;
                case SyncComms::CapturePhase::Paused:     phase = "paused"; break;
                case SyncComms::CapturePhase::Finalizing: phase = "finalizing"; break;
                case SyncComms::CapturePhase::Error:      phase = "error"; break;
            }
            nlohmann::json payload = {
                {"phase", phase},
                {"matchGuid", s.matchGuid},
                {"segmentCount", s.segmentCount},
                {"segmentSeconds", s.segmentSeconds},
                {"lastError", s.lastError},
            };
            pushToJs(
                "if (typeof window.__onCaptureStatus === 'function') "
                "window.__onCaptureStatus(" + payload.dump() + ");"
            );
        };
        orchestrator.SetStatusCallback(pushCaptureStatus);

        w.bind("debugLastUpdateState",
               [&orchestrator](const std::string& /* args_json */) -> std::string {
                   auto s = orchestrator.GetLastUpdateStateRaw();
                   return s.empty() ? "null" : s;
               });

        stats.SetEventCallback([&pushToJs, &orchestrator, &playback](const std::string& jsonEvent) {
            // Wire format from the Stats API uses TitleCase ("Event", "Data")
            // and `Data` is a *JSON-encoded string*, not a JS object. We
            // normalize here so the frontend sees a stable shape:
            //   { event: <string>, data: <object> }
            // regardless of casing or nested-string quirks. Mirrors the
            // normalize_event() in tools/stats_api_probe.py.
            std::string normalized;
            try {
                auto j = nlohmann::json::parse(jsonEvent);
                nlohmann::json out;
                if      (j.contains("Event")) out["event"] = j["Event"];
                else if (j.contains("event")) out["event"] = j["event"];
                else if (j.contains("name"))  out["event"] = j["name"];
                else                          out["event"] = "<unknown>";

                nlohmann::json data = j.contains("Data") ? j["Data"]
                                    : j.contains("data") ? j["data"]
                                    : nlohmann::json::object();
                if (data.is_string()) {
                    try {
                        data = nlohmann::json::parse(data.get<std::string>());
                    } catch (...) {
                        // Some emitters legitimately send Data as a string —
                        // wrap so the frontend can still see the value.
                        data = nlohmann::json{{"raw", data.get<std::string>()}};
                    }
                }
                if (!data.is_object() && !data.is_array()) {
                    data = nlohmann::json::object();
                }
                out["data"] = data;
                normalized = out.dump();

                // Hand off to capture + playback on the worker thread. Both
                // own their own mutexes and are safe to call here.
                std::string evtName = out["event"].is_string()
                                       ? out["event"].get<std::string>()
                                       : "";
                std::string dataStr = data.dump();
                orchestrator.HandleEvent(evtName, dataStr);
                playback.HandleEvent(evtName, dataStr);
            } catch (...) {
                normalized = R"({"event":"<parse-error>","data":{}})";
            }
            pushToJs(
                "if (typeof window.__onRlEvent === 'function') "
                "window.__onRlEvent(" + normalized + ");"
            );
        });

        stats.SetStateCallback([&pushToJs](SyncComms::StatsApiConnectionState s) {
            const char* label = "disconnected";
            switch (s) {
                case SyncComms::StatsApiConnectionState::Disconnected: label = "disconnected"; break;
                case SyncComms::StatsApiConnectionState::Connecting:   label = "connecting";   break;
                case SyncComms::StatsApiConnectionState::Connected:    label = "connected";    break;
            }
            pushToJs(
                "if (typeof window.__onRlState === 'function') "
                "window.__onRlState('" + std::string(label) + "');"
            );
        });

        // Bind for the frontend to query current connection state on startup
        // — eliminates the race where C++ fires Connecting/Connected before
        // the JS __onRlState handler exists.
        w.bind("getStatsApiState",
               [&stats](const std::string& /* args_json */) -> std::string {
                   const char* label = "disconnected";
                   switch (stats.GetState()) {
                       case SyncComms::StatsApiConnectionState::Disconnected: label = "disconnected"; break;
                       case SyncComms::StatsApiConnectionState::Connecting:   label = "connecting";   break;
                       case SyncComms::StatsApiConnectionState::Connected:    label = "connected";    break;
                   }
                   return std::string("\"") + label + "\"";
               });

        w.bind("getPlaybackStatus",
               [&playback](const std::string&) -> std::string {
                   auto s = playback.GetStatus();
                   const char* phase = "idle";
                   switch (s.phase) {
                       case SyncComms::PlaybackPhase::Idle:             phase = "idle"; break;
                       case SyncComms::PlaybackPhase::SearchingSidecar: phase = "searching"; break;
                       case SyncComms::PlaybackPhase::NoSidecar:        phase = "noSidecar"; break;
                       case SyncComms::PlaybackPhase::Playing:          phase = "playing"; break;
                       case SyncComms::PlaybackPhase::Error:            phase = "error"; break;
                   }
                   nlohmann::json payload = {
                       {"phase", phase},
                       {"matchGuid", s.matchGuid},
                       {"sidecarPath", s.sidecarPath},
                       {"replayPath", s.replayPath},
                       {"segmentCount", s.segmentCount},
                       {"activeSegmentIndex", s.activeSegmentIndex},
                       {"elapsedSec", s.elapsedSec},
                       {"anchored", s.anchored},
                       {"lastError", s.lastError},
                   };
                   return payload.dump();
               });

        // Pull initial capture status so the UI doesn't briefly show "idle"
        // when in-fact we're already past startup.
        w.bind("getCaptureStatus",
               [&orchestrator](const std::string& /* args_json */) -> std::string {
                   auto s = orchestrator.GetStatus();
                   const char* phase = "idle";
                   switch (s.phase) {
                       case SyncComms::CapturePhase::Idle:       phase = "idle"; break;
                       case SyncComms::CapturePhase::Armed:      phase = "armed"; break;
                       case SyncComms::CapturePhase::Recording:  phase = "recording"; break;
                       case SyncComms::CapturePhase::Paused:     phase = "paused"; break;
                       case SyncComms::CapturePhase::Finalizing: phase = "finalizing"; break;
                       case SyncComms::CapturePhase::Error:      phase = "error"; break;
                   }
                   nlohmann::json payload = {
                       {"phase", phase},
                       {"matchGuid", s.matchGuid},
                       {"segmentCount", s.segmentCount},
                       {"segmentSeconds", s.segmentSeconds},
                       {"lastError", s.lastError},
                   };
                   return payload.dump();
               });

        // ── Startup housekeeping ─────────────────────────────────────────────
        // Run BEFORE stats.Start() so no capture can be in flight: the sweep
        // would otherwise be unable to tell a just-created in-progress WAV from
        // a leftover. The orphan sweep is cheap (a directory scan); do it
        // synchronously. Retention parses every .replay header to judge
        // boundness, which is slower, so run it on a detached thread to keep
        // window startup snappy — it only touches old (>2wk) sidecars that
        // nothing else races.
        {
            const std::string outDir = liveConfig.GetOutputDir();
            auto maintLog = [](const std::string& m) { std::cerr << m << "\n"; };
            SyncComms::SweepOrphanWavs(outDir, maintLog);

            std::thread([outDir]() {
                try {
                    SyncComms::ReplayLocator locator;
                    SyncComms::PruneUnboundSidecars(
                        outDir, locator, SyncComms::kRetentionDays,
                        [](const std::string& m) { std::cerr << m << "\n"; });
                } catch (...) {
                    // Maintenance is best-effort; never take the app down.
                }
            }).detach();
        }

        stats.Start();

        FrontendSource fs = ResolveFrontend();
        switch (fs.kind) {
            case FrontendSource::Kind::DevUrl:
                w.navigate(fs.payload);
                break;
            case FrontendSource::Kind::InlineHtml:
                w.set_html(fs.payload);
                break;
            case FrontendSource::Kind::NotFound:
                w.set_html(kFallbackHtml);
                break;
        }

        w.run();
        stats.Stop();
        orchestrator.Shutdown();
        playback.Shutdown();
        return 0;
    } catch (const webview::exception& e) {
        MessageBoxA(nullptr, e.what(), "SyncComms — webview error", MB_OK | MB_ICONERROR);
        return 1;
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "SyncComms — fatal", MB_OK | MB_ICONERROR);
        return 1;
    }
}
