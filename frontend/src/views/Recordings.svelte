<script lang="ts">
  import { isInShell } from "../lib/bridge";
  import { app } from "../lib/state.svelte";
  import type { RecordingInfo } from "../lib/types";

  $effect(() => {
    app.ensureRecordingsLoaded();
  });

  // Refresh after a finalize phase: as soon as the orchestrator transitions
  // out of "finalizing" back to "idle", a new sidecar may have been written.
  let lastPhase = $state(app.capture.phase);
  $effect(() => {
    const cur = app.capture.phase;
    if (lastPhase === "finalizing" && cur === "idle") {
      void app.refreshRecordings();
    }
    lastPhase = cur;
  });

  function fmtBytes(n: number): string {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
    return `${(n / (1024 * 1024)).toFixed(2)} MB`;
  }
  function fmtDuration(s: number): string {
    if (!isFinite(s) || s <= 0) return "0:00";
    const m = Math.floor(s / 60);
    const sec = Math.floor(s % 60);
    return `${m}:${sec.toString().padStart(2, "0")}`;
  }
  function fmtDate(unix: number): string {
    if (!unix) return "—";
    return new Date(unix * 1000).toLocaleString(undefined, {
      month: "short", day: "numeric", hour: "2-digit", minute: "2-digit",
    });
  }
  function shortGuid(g: string): string {
    return g ? g.slice(0, 12) + "…" : "—";
  }

  // Two-step delete: first click arms the row (keyed by filePath, which is
  // unique), second confirms. Avoids depending on a native confirm() dialog,
  // which some WebView2 configs suppress.
  let pendingDelete = $state<string | null>(null);
  let deleting = $state<string | null>(null);

  async function confirmDelete(r: RecordingInfo) {
    deleting = r.filePath;
    try {
      await app.deleteRecording(r.matchGuid);
    } finally {
      deleting = null;
      pendingDelete = null;
    }
  }

  // Bucket "Recent" (< 30 days) vs "Archived" (≥ 30 days).
  const now = $derived(Date.now() / 1000);
  const buckets = $derived.by(() => {
    const recent: RecordingInfo[] = [];
    const archived: RecordingInfo[] = [];
    for (const r of app.recordings) {
      const ageDays = (now - r.modifiedAtUnix) / (60 * 60 * 24);
      (ageDays < 30 ? recent : archived).push(r);
    }
    return { recent, archived };
  });
</script>

<header class="page-header">
  <h2>Recordings</h2>
  <p class="subtitle">
    Captured audio sidecars stored locally. Newest first.
    <button class="link" onclick={() => app.refreshRecordings()} disabled={app.loadingRecordings || !isInShell()}>
      {app.loadingRecordings ? "Refreshing..." : "Refresh"}
    </button>
  </p>
</header>

{#if app.recordingsError}
  <div class="error">bridge error: {app.recordingsError}</div>
{/if}

{#if buckets.recent.length === 0 && buckets.archived.length === 0 && !app.loadingRecordings}
  <div class="card empty-card">
    <div class="empty-title">No recordings yet</div>
    <div class="empty-body">
      Audio sidecars will appear here after a match finishes recording. Make
      sure recording is enabled in Settings, and that Rocket League is running
      with the Stats API enabled.
    </div>
  </div>
{/if}

{#snippet recRow(r: RecordingInfo, archived: boolean)}
  <li class="rec" class:rec-archived={archived}>
    <div class="rec-main">
      <div class="rec-line">
        <span class="rec-guid mono">{shortGuid(r.matchGuid)}</span>
        <span class="rec-when">{fmtDate(r.modifiedAtUnix)}</span>
      </div>
      <div class="rec-meta">
        {r.segmentCount} segment{r.segmentCount === 1 ? "" : "s"}
        · {fmtDuration(r.totalDurationSec)}
        · {fmtBytes(r.fileSize)}
      </div>
    </div>
    <div class="rec-actions">
      {#if pendingDelete === r.filePath}
        <button class="btn-confirm" disabled={deleting === r.filePath} onclick={() => confirmDelete(r)}>
          {deleting === r.filePath ? "Deleting…" : "Confirm"}
        </button>
        <button class="btn-cancel" disabled={deleting === r.filePath} onclick={() => (pendingDelete = null)}>
          Cancel
        </button>
      {:else}
        <button class="btn-delete" title="Delete recording" onclick={() => (pendingDelete = r.filePath)}>
          Delete
        </button>
      {/if}
    </div>
  </li>
{/snippet}

{#if buckets.recent.length > 0}
  <section class="group">
    <h3>Recent</h3>
    <ul class="rec-list">
      {#each buckets.recent as r (r.matchGuid + r.modifiedAtUnix)}
        {@render recRow(r, false)}
      {/each}
    </ul>
  </section>
{/if}

{#if buckets.archived.length > 0}
  <section class="group">
    <h3>Archived · 30+ days old</h3>
    <ul class="rec-list">
      {#each buckets.archived as r (r.matchGuid + r.modifiedAtUnix)}
        {@render recRow(r, true)}
      {/each}
    </ul>
  </section>
{/if}

<style>
  .page-header { margin-bottom: 20px; }
  h2 { margin: 0; font-size: 22px; font-weight: 600; letter-spacing: -0.015em; }
  .subtitle { margin: 4px 0 0; color: var(--muted); font-size: 13px; }
  .link {
    background: transparent; border: 0; color: var(--accent); cursor: pointer;
    font: inherit; padding: 0 6px; border-radius: 3px;
  }
  .link:hover:not(:disabled) { background: var(--panel-2); }
  .link:disabled { opacity: 0.5; cursor: not-allowed; }

  .card {
    background: var(--panel); border: 1px solid var(--border);
    border-radius: 10px; padding: 24px;
  }
  .empty-card { text-align: center; }
  .empty-title { font-weight: 600; margin-bottom: 6px; }
  .empty-body { color: var(--muted); max-width: 480px; margin: 0 auto; font-size: 13px; }

  .group { margin-bottom: 20px; }
  h3 {
    margin: 0 0 8px; font-size: 12px; font-weight: 600;
    color: var(--muted); text-transform: uppercase; letter-spacing: 0.06em;
  }

  .rec-list { list-style: none; padding: 0; margin: 0; display: flex; flex-direction: column; gap: 4px; }
  .rec {
    background: var(--panel); border: 1px solid var(--border);
    border-radius: 8px; padding: 10px 14px;
    display: flex; align-items: center; justify-content: space-between; gap: 12px;
    transition: border-color 0.12s;
  }
  .rec:hover { border-color: var(--border-strong); }
  .rec-archived { opacity: 0.7; }

  .rec-main { display: flex; flex-direction: column; gap: 2px; min-width: 0; flex: 1; }
  .rec-line { display: flex; gap: 12px; align-items: baseline; }
  .rec-guid { font-size: 12.5px; color: var(--fg-dim); }
  .rec-when { font-size: 12px; color: var(--muted); }
  .rec-meta { font-size: 11.5px; color: var(--muted); }

  .rec-actions { display: flex; gap: 6px; flex-shrink: 0; }
  .rec-actions button {
    background: transparent; border: 1px solid var(--border);
    color: var(--muted); cursor: pointer; font: inherit; font-size: 12px;
    padding: 4px 10px; border-radius: 6px; transition: all 0.12s;
  }
  .rec-actions button:disabled { opacity: 0.5; cursor: default; }
  .btn-delete:hover:not(:disabled) {
    border-color: var(--danger-border); color: var(--danger);
  }
  .btn-confirm {
    border-color: var(--danger-border) !important;
    color: var(--danger) !important; background: var(--danger-bg) !important;
  }
  .btn-confirm:hover:not(:disabled) { filter: brightness(1.15); }
  .btn-cancel:hover:not(:disabled) { border-color: var(--border-strong); color: var(--fg-dim); }

  .mono { font-family: ui-monospace, "SF Mono", Consolas, monospace; }

  .error {
    background: var(--danger-bg); border: 1px solid var(--danger-border);
    color: var(--danger); border-radius: 8px; padding: 12px 14px;
    font-family: ui-monospace, monospace; font-size: 12.5px;
    margin-bottom: 16px;
  }
</style>
