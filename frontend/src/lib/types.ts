// Shared types for the C++↔TS bridge.

export interface AudioProcessInfo {
  pid: number;
  processName: string;
  displayName: string;
}

export interface MicrophoneInfo {
  /** Stable PnP-style endpoint id; use this to persist a selection. */
  deviceId: string;
  /** User-visible name (e.g. "Microphone (Razer Seiren X)"). */
  friendlyName: string;
  /** True if this is the system default communications microphone. */
  isDefault: boolean;
}

// Persisted user settings. Schema is owned here (frontend) and written through
// the bridge as opaque JSON to %APPDATA%\SyncComms\settings.json. Add fields
// freely; missing fields fall back to defaults defined in state.svelte.ts.
export interface AppSettings {
  /** Master on/off switch — when false the capture pipeline never engages,
   *  even with a connected Stats API and a live match. */
  recordingEnabled: boolean;
  /** Process name (e.g. "Discord.exe") whose audio output we capture, or null
   *  for the system default render device (capture everything). */
  captureTarget: string | null;
  /** Whether to mix microphone input into the captured audio. */
  includeMic: boolean;
  /** Endpoint id of the selected microphone, or null = system default. Only
   *  meaningful when includeMic is true. */
  microphoneId: string | null;
}

// ── Capture pipeline state (pushed by C++ CaptureOrchestrator) ─────────────

export type CapturePhase =
  | "idle"        // not recording; waiting for kickoff
  | "armed"       // match created, waiting for countdown
  | "recording"   // actively capturing audio
  | "paused"      // match paused, capture paused
  | "finalizing"  // match ended, compressing + writing sidecar
  | "error";      // last attempt failed; see lastError

export interface CaptureStatus {
  phase: CapturePhase;
  matchGuid: string | null;
  segmentCount: number;
  /** Seconds since the current segment started capturing (0 when not active). */
  segmentSeconds: number;
  lastError: string | null;
}

// ── Recordings index ───────────────────────────────────────────────────────

// ── Playback (replay-sync) ─────────────────────────────────────────────────

export type PlaybackPhase =
  | "idle"        // not in a replay viewer
  | "searching"   // looking for a sidecar matching the current MatchGuid
  | "noSidecar"   // replay open but no audio for it
  | "playing"     // audio synced to replay clock
  | "error";

export interface PlaybackStatus {
  phase: PlaybackPhase;
  matchGuid: string;
  sidecarPath: string;
  /** Path to the .replay file we matched by MatchGuid; empty if unmatched. */
  replayPath: string;
  segmentCount: number;
  /** 0-based index of the segment currently playing, or -1 between segments. */
  activeSegmentIndex: number;
  elapsedSec: number;
  /** True when segments are aligned to the canonical replay timeline using
   *  goal frames + total frame count from the .replay header. False when we
   *  fall back to the elapsedFloor calibration heuristic (no .replay found,
   *  parse failed, or segment count didn't match goalCount + 1). */
  anchored: boolean;
  lastError: string;
}

export interface RecordingInfo {
  matchGuid: string;
  filePath: string;
  fileSize: number;
  /** Unix timestamp (seconds) of last write — proxy for "when recorded". */
  modifiedAtUnix: number;
  segmentCount: number;
  totalDurationSec: number;
}

// ── Stats API event stream ─────────────────────────────────────────────────

export type StatsApiConnectionState = "disconnected" | "connecting" | "connected";

/** Envelope shape pushed from the C++ StatsApiClient. The `data` field's
 *  schema varies per `event` name — we keep it as `unknown` and let consumers
 *  narrow as needed. The Python probe documented the names we actually care
 *  about: MatchCreated/CountdownBegin/StatfeedEvent/MatchEnded/UpdateState/
 *  ReplayCreated/MatchPaused/MatchUnpaused/MatchDestroyed. */
export interface RlEvent {
  event: string;
  data: Record<string, unknown>;
}
