// App-wide reactive state. Lives outside any component so it survives view
// switches (Svelte unmounts a component when its parent {#if} branch flips,
// which would discard any local $state).
//
// Everything here is a singleton: we export `app` and components reach into
// it directly. Two concerns are split inside the class:
//   - `settings` (and friends): persisted to disk via the C++ bridge.
//   - `processes` cache: in-memory only; refresh button rehydrates it.

import {
  deleteRecording,
  getCaptureStatus,
  getPlaybackStatus,
  getStatsApiState,
  isInShell,
  listAudioProcesses,
  listMicrophones,
  listRecordings,
  loadSettings,
  onCaptureStatus,
  onPlaybackStatus,
  onRlEvent,
  onRlState,
  saveSettings,
} from "./bridge";
import type {
  AppSettings,
  AudioProcessInfo,
  CaptureStatus,
  MicrophoneInfo,
  PlaybackStatus,
  RecordingInfo,
  RlEvent,
  StatsApiConnectionState,
} from "./types";

const DEFAULT_CAPTURE_STATUS: CaptureStatus = {
  phase: "idle",
  matchGuid: null,
  segmentCount: 0,
  segmentSeconds: 0,
  lastError: null,
};

const DEFAULT_PLAYBACK_STATUS: PlaybackStatus = {
  phase: "idle",
  matchGuid: "",
  sidecarPath: "",
  replayPath: "",
  segmentCount: 0,
  activeSegmentIndex: -1,
  elapsedSec: 0,
  anchored: false,
  lastError: "",
};

const DEFAULT_SETTINGS: AppSettings = {
  recordingEnabled: true,
  captureTarget: null, // null = system audio
  includeMic: false,
  microphoneId: null, // null = system default communications mic
};

export interface RlEventEntry {
  id: number;
  ts: Date;
  event: string;
  data: Record<string, unknown>;
}

export interface GoalEntry {
  scorer: string;
  team: number;
  at: Date;
  /** Elapsed match time when the goal happened, in seconds. */
  elapsed: number;
}

export interface MatchSnapshot {
  matchGuid: string;
  startedAt: Date;
  isReplay: boolean;
  elapsed: number;
  frame: number;
  goals: GoalEntry[];
  paused: boolean;
  ended: boolean;
}

class AppState {
  // ── Persisted settings ───────────────────────────────────────────────────
  settings = $state<AppSettings>({ ...DEFAULT_SETTINGS });
  /** True after the initial loadSettings() has resolved (success or not). */
  settingsLoaded = $state(false);
  /** Last error from loadSettings/saveSettings, if any. */
  settingsError = $state<string | null>(null);

  // ── Audio sessions cache (not persisted) ─────────────────────────────────
  processes = $state<AudioProcessInfo[]>([]);
  loadingProcesses = $state(false);
  processesError = $state<string | null>(null);
  lastProcessRefresh = $state<Date | null>(null);
  /** First-load gate so navigation between views doesn't re-fetch every time. */
  private processesLoadedOnce = false;

  // ── Microphone enumeration cache (not persisted) ─────────────────────────
  microphones = $state<MicrophoneInfo[]>([]);
  loadingMicrophones = $state(false);
  microphonesError = $state<string | null>(null);
  private micsLoadedOnce = false;

  // ── Recordings cache (not persisted) ─────────────────────────────────────
  recordings = $state<RecordingInfo[]>([]);
  loadingRecordings = $state(false);
  recordingsError = $state<string | null>(null);
  lastRecordingsRefresh = $state<Date | null>(null);
  private recordingsLoadedOnce = false;

  // ── Stats API live state ─────────────────────────────────────────────────
  connection = $state<StatsApiConnectionState>("disconnected");
  capture = $state<CaptureStatus>({ ...DEFAULT_CAPTURE_STATUS });
  playback = $state<PlaybackStatus>({ ...DEFAULT_PLAYBACK_STATUS });

  /** Most recent N events, newest first. Bounded so a long match doesn't
   *  balloon memory — we cap and drop the oldest entries. */
  events = $state<RlEventEntry[]>([]);
  private static readonly MAX_EVENTS = 200;

  /** Headline live-match summary derived from the event stream. Updated as
   *  events arrive — null when there is no active match. */
  match = $state<MatchSnapshot | null>(null);

  constructor() {
    if (isInShell()) {
      // Fire-and-forget; UI shows defaults until this resolves (microseconds).
      void this.hydrate();
      onRlState((s) => { this.connection = s; });
      onRlEvent((e) => { this.handleRlEvent(e); });
      onCaptureStatus((s) => { this.capture = s; });
      onPlaybackStatus((s) => { this.playback = s; });
      // Cover startup race: C++ may have fired push events before our
      // handlers existed. Pull current state explicitly.
      void this.refreshConnectionState();
      void this.refreshCaptureStatus();
      void this.refreshPlaybackStatus();
    } else {
      this.settingsLoaded = true; // dev-in-browser: skip bridge
    }
  }

  private async refreshConnectionState() {
    try {
      this.connection = await getStatsApiState();
    } catch (e) {
      console.warn("[state] getStatsApiState failed:", e);
    }
  }

  private async refreshCaptureStatus() {
    try {
      this.capture = await getCaptureStatus();
    } catch (e) {
      console.warn("[state] getCaptureStatus failed:", e);
    }
  }

  private async refreshPlaybackStatus() {
    try {
      this.playback = await getPlaybackStatus();
    } catch (e) {
      console.warn("[state] getPlaybackStatus failed:", e);
    }
  }

  private handleRlEvent(e: RlEvent) {
    // Append to the rolling log (newest first, capped).
    const entry: RlEventEntry = {
      id: ++this.eventId,
      ts: new Date(),
      event: e.event,
      data: e.data,
    };
    this.events = [entry, ...this.events].slice(0, AppState.MAX_EVENTS);

    // Maintain a derived live-match snapshot. We touch only fields the UI
    // surfaces; raw event data stays in `events` for anyone who wants more.
    const data = e.data ?? {};
    const guid = (data.MatchGuid ?? data.matchGuid) as string | undefined;

    switch (e.event) {
      case "MatchCreated":
      case "MatchInitialized":
      case "CountdownBegin":
        if (guid) {
          if (!this.match || this.match.matchGuid !== guid) {
            this.match = {
              matchGuid: guid,
              startedAt: new Date(),
              isReplay: false,
              elapsed: 0,
              frame: 0,
              goals: [],
              paused: false,
              ended: false,
            };
          }
        }
        break;

      case "MatchPaused":
        if (this.match) this.match = { ...this.match, paused: true };
        break;
      case "MatchUnpaused":
        if (this.match) this.match = { ...this.match, paused: false };
        break;

      case "GoalScored": {
        // Canonical goal event (richer than StatfeedEvent[Goal]). Two quirks
        // handled here, both confirmed from recorded Stats API streams:
        //  - It fires TWICE per goal: the real one (live, has a Scorer and
        //    GoalTime > 0) and a cinematic re-fire (empty Scorer, GoalTime 0).
        //    Only the real one becomes a goal entry.
        //  - The Stats API sends NO Elapsed during live play, so we can't use
        //    match.elapsed for the goal time (it stays 0 — that was the
        //    "every goal shows 0:00" bug). GoalTime is the length of the round
        //    that just ended, so the running sum is the match play-time at
        //    each goal — matching the sidecar's per-goal stamps.
        if (!this.match) break;
        const scorer = (data.Scorer ?? {}) as Record<string, unknown>;
        const scorerName = String(scorer.Name ?? "");
        const goalTime = Number(data.GoalTime ?? 0);
        if (!scorerName || !(goalTime > 0)) break;
        const prevElapsed = this.match.goals.length
          ? this.match.goals[this.match.goals.length - 1].elapsed
          : 0;
        this.match = {
          ...this.match,
          goals: [
            ...this.match.goals,
            {
              scorer: scorerName,
              team: Number(scorer.TeamNum ?? 0),
              at: new Date(),
              elapsed: prevElapsed + goalTime,
            },
          ],
        };
        break;
      }

      case "UpdateState":
      case "GameState": {
        if (!this.match) break;
        const game = (data.Game ?? data.game ?? {}) as Record<string, unknown>;
        const elapsed = Number(game.Elapsed ?? data.Elapsed ?? this.match.elapsed);
        const frame = Number(game.Frame ?? data.Frame ?? this.match.frame);
        const isReplay = Boolean(game.bReplay ?? data.bReplay ?? this.match.isReplay);
        if (
          elapsed !== this.match.elapsed ||
          frame !== this.match.frame ||
          isReplay !== this.match.isReplay
        ) {
          this.match = { ...this.match, elapsed, frame, isReplay };
        }
        break;
      }

      case "MatchEnded":
        if (this.match) this.match = { ...this.match, ended: true };
        break;

      case "MatchDestroyed":
        // Keep the snapshot around briefly (so the UI doesn't flash empty)
        // — `match` is cleared when a new MatchCreated arrives.
        break;
    }
  }
  private eventId = 0;

  private async hydrate() {
    try {
      const loaded = await loadSettings();
      if (loaded && typeof loaded === "object") {
        this.settings = { ...DEFAULT_SETTINGS, ...loaded };
      }
    } catch (e) {
      this.settingsError = e instanceof Error ? e.message : String(e);
      console.warn("[state] loadSettings failed:", e);
    } finally {
      this.settingsLoaded = true;
    }
  }

  /** Update settings and persist immediately. */
  async updateSettings(patch: Partial<AppSettings>) {
    this.settings = { ...this.settings, ...patch };
    if (!isInShell()) return;
    try {
      await saveSettings(this.settings);
      this.settingsError = null;
    } catch (e) {
      this.settingsError = e instanceof Error ? e.message : String(e);
      console.error("[state] saveSettings failed:", e);
    }
  }

  /** Convenience: change capture target and persist. */
  setCaptureTarget(name: string | null) {
    return this.updateSettings({ captureTarget: name });
  }

  setIncludeMic(on: boolean) {
    return this.updateSettings({ includeMic: on });
  }

  setRecordingEnabled(on: boolean) {
    return this.updateSettings({ recordingEnabled: on });
  }

  setMicrophoneId(id: string | null) {
    return this.updateSettings({ microphoneId: id });
  }

  /** Force-refresh the audio session list from the OS. */
  async refreshProcesses() {
    if (this.loadingProcesses) return;
    this.loadingProcesses = true;
    this.processesError = null;
    try {
      this.processes = await listAudioProcesses();
      this.lastProcessRefresh = new Date();
      this.processesLoadedOnce = true;
    } catch (e) {
      this.processesError = e instanceof Error ? e.message : String(e);
    } finally {
      this.loadingProcesses = false;
    }
  }

  /** Lazy first load — call from a view's onMount to populate the cache. */
  ensureProcessesLoaded() {
    if (!this.processesLoadedOnce && !this.loadingProcesses && isInShell()) {
      void this.refreshProcesses();
    }
  }

  async refreshMicrophones() {
    if (this.loadingMicrophones) return;
    this.loadingMicrophones = true;
    this.microphonesError = null;
    try {
      this.microphones = await listMicrophones();
      this.micsLoadedOnce = true;
    } catch (e) {
      this.microphonesError = e instanceof Error ? e.message : String(e);
    } finally {
      this.loadingMicrophones = false;
    }
  }

  ensureMicrophonesLoaded() {
    if (!this.micsLoadedOnce && !this.loadingMicrophones && isInShell()) {
      void this.refreshMicrophones();
    }
  }

  async refreshRecordings() {
    if (this.loadingRecordings) return;
    this.loadingRecordings = true;
    this.recordingsError = null;
    try {
      this.recordings = await listRecordings();
      this.lastRecordingsRefresh = new Date();
      this.recordingsLoadedOnce = true;
    } catch (e) {
      this.recordingsError = e instanceof Error ? e.message : String(e);
    } finally {
      this.loadingRecordings = false;
    }
  }

  ensureRecordingsLoaded() {
    if (!this.recordingsLoadedOnce && !this.loadingRecordings && isInShell()) {
      void this.refreshRecordings();
    }
  }

  /** Delete a recording by replay id. Removes it from the list optimistically;
   *  on failure, re-reads from disk to restore the truthful state. */
  async deleteRecording(matchGuid: string) {
    if (!isInShell()) return;
    const before = this.recordings;
    this.recordings = before.filter((r) => r.matchGuid !== matchGuid);
    try {
      const ok = await deleteRecording(matchGuid);
      if (!ok) await this.refreshRecordings();
    } catch (e) {
      this.recordingsError = e instanceof Error ? e.message : String(e);
      await this.refreshRecordings();
    }
  }
}

export const app = new AppState();
