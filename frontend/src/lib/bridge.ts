// Typed wrappers around the native (C++) functions exposed via webview's
// `bind()`. The native side `set_html`/JS bridge attaches each bound function
// to `window.<name>`. To keep the C++ ↔ TS contract centralized, all calls go
// through this module.
//
// The webview library auto-parses every C++ return value as JSON before the
// JS Promise resolves. So a C++ side that does `arr.dump()` returning an
// array surfaces in JS as a real array; one that returns `"\"connected\""`
// (the JSON encoding of the string "connected") surfaces as the string
// "connected". Wrappers below just cast.

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

// Augment Window with the native bindings the C++ side registers. Add new
// entries here whenever you add a new `webview.bind(...)` in main.cpp.
//
// `__onRlEvent` and `__onRlState` are NOT bind()s — they're receivers that
// the C++ StatsApiClient calls into via webview.eval(). Frontend assigns to
// them on startup; if not assigned, the C++ side's eval() is a no-op (it
// guards with `typeof === "function"`).
declare global {
  interface Window {
    listAudioProcesses?: () => Promise<unknown>;
    listMicrophones?: () => Promise<unknown>;
    loadSettings?: () => Promise<unknown>;
    saveSettings?: (settings: AppSettings) => Promise<unknown>;
    getStatsApiState?: () => Promise<unknown>;
    getCaptureStatus?: () => Promise<unknown>;
    getPlaybackStatus?: () => Promise<unknown>;
    listRecordings?: () => Promise<unknown>;
    deleteRecording?: (matchGuid: string) => Promise<unknown>;
    __onRlEvent?: (event: RlEvent) => void;
    __onRlState?: (state: StatsApiConnectionState) => void;
    __onCaptureStatus?: (status: CaptureStatus) => void;
    __onPlaybackStatus?: (status: PlaybackStatus) => void;
  }
}

function requireBinding<K extends keyof Window>(name: K): NonNullable<Window[K]> {
  const fn = window[name];
  if (!fn) {
    throw new Error(
      `bridge: native binding "${String(name)}" is not registered. ` +
        `Are you running outside the SyncComms shell?`
    );
  }
  return fn as NonNullable<Window[K]>;
}

export async function listAudioProcesses(): Promise<AudioProcessInfo[]> {
  const fn = requireBinding("listAudioProcesses");
  return (await fn()) as AudioProcessInfo[];
}

export async function listMicrophones(): Promise<MicrophoneInfo[]> {
  const fn = requireBinding("listMicrophones");
  return (await fn()) as MicrophoneInfo[];
}

/** Returns the persisted settings, or null if nothing has been saved yet. */
export async function loadSettings(): Promise<Partial<AppSettings> | null> {
  const fn = requireBinding("loadSettings");
  return (await fn()) as Partial<AppSettings> | null;
}

/** Persist the full settings object. Backend writes atomically to disk. */
export async function saveSettings(settings: AppSettings): Promise<void> {
  const fn = requireBinding("saveSettings");
  await fn(settings);
}

/** Ask C++ for the current Stats API connection state. Used at startup to
 *  recover from the race where C++ fires push events before the JS handlers
 *  are registered. */
export async function getStatsApiState(): Promise<StatsApiConnectionState> {
  const fn = requireBinding("getStatsApiState");
  return (await fn()) as StatsApiConnectionState;
}

/** Pull the current capture orchestrator status. */
export async function getCaptureStatus(): Promise<CaptureStatus> {
  const fn = requireBinding("getCaptureStatus");
  return (await fn()) as CaptureStatus;
}

/** Pull the current playback controller status. */
export async function getPlaybackStatus(): Promise<PlaybackStatus> {
  const fn = requireBinding("getPlaybackStatus");
  return (await fn()) as PlaybackStatus;
}

/** List sidecars in the recordings folder (no audio data — metadata only). */
export async function listRecordings(): Promise<RecordingInfo[]> {
  const fn = requireBinding("listRecordings");
  return (await fn()) as RecordingInfo[];
}

/** Delete one recording (sidecar + referenced WAVs) by its replay id. Resolves
 *  to true when the backend found and removed a sidecar. */
export async function deleteRecording(matchGuid: string): Promise<boolean> {
  const fn = requireBinding("deleteRecording");
  return (await fn(matchGuid)) as boolean;
}

/** True when running inside the SyncComms WebView2 shell (has bindings). */
export function isInShell(): boolean {
  return typeof window.listAudioProcesses === "function";
}

/** Subscribe to real-time RL events pushed from the C++ StatsApiClient.
 *  Returns an unsubscribe function. Multiple listeners are supported via a
 *  fan-out: we maintain a private array on `window.__onRlEvent`. */
export function onRlEvent(handler: (e: RlEvent) => void): () => void {
  return installFanout("__onRlEvent", handler);
}

/** Subscribe to connection state changes (disconnected/connecting/connected).
 *  Same fan-out pattern as onRlEvent. */
export function onRlState(handler: (s: StatsApiConnectionState) => void): () => void {
  return installFanout("__onRlState", handler);
}

/** Subscribe to capture orchestrator status changes. */
export function onCaptureStatus(handler: (s: CaptureStatus) => void): () => void {
  return installFanout("__onCaptureStatus", handler);
}

/** Subscribe to playback controller status changes. */
export function onPlaybackStatus(handler: (s: PlaybackStatus) => void): () => void {
  return installFanout("__onPlaybackStatus", handler);
}

// Helper: install a fan-out receiver on window so multiple consumers can
// subscribe. The C++ side calls `window.__onRlEvent(...)` once per event and
// our wrapper forwards to every registered handler.
function installFanout<K extends "__onRlEvent" | "__onRlState" | "__onCaptureStatus" | "__onPlaybackStatus">(
  key: K,
  handler: (arg: any) => void
): () => void {
  const w = window as any;
  if (!w[`${key}__handlers`]) {
    const handlers: Array<(arg: any) => void> = [];
    w[`${key}__handlers`] = handlers;
    w[key] = (arg: any) => {
      for (const h of handlers) {
        try { h(arg); } catch (e) { console.error(`[bridge] ${key} handler threw`, e); }
      }
    };
  }
  const handlers: Array<(arg: any) => void> = w[`${key}__handlers`];
  handlers.push(handler);
  return () => {
    const i = handlers.indexOf(handler);
    if (i >= 0) handlers.splice(i, 1);
  };
}
