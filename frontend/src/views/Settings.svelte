<script lang="ts">
  import { isInShell } from "../lib/bridge";
  import { app } from "../lib/state.svelte";

  // Lazy-load both lists on first view. The store remembers it's been loaded
  // once, so re-mounts from nav don't refetch.
  $effect(() => {
    app.ensureProcessesLoaded();
    app.ensureMicrophonesLoaded();
  });
</script>

<header class="page-header">
  <h2>Settings</h2>
  <p class="subtitle">Capture preferences and audio sources</p>
</header>

<section class="card">
  <header class="card-header">
    <div class="card-title-row">
      <div>
        <h3>Recording</h3>
        <p class="card-sub">
          Master switch. When off, the app stays connected to the Stats API
          for monitoring but never engages capture, even during a live match.
        </p>
      </div>
      <label class="switch">
        <input
          type="checkbox"
          checked={app.settings.recordingEnabled}
          onchange={(e) => app.setRecordingEnabled((e.target as HTMLInputElement).checked)}
        />
        <span class="switch-track"><span class="switch-thumb"></span></span>
      </label>
    </div>
  </header>
</section>

<section class="card">
  <header class="card-header">
    <div>
      <h3>Audio capture target</h3>
      <p class="card-sub">
        Pick the process whose output should be captured (e.g. Discord). Leave
        on System Audio to capture everything from the default device.
      </p>
    </div>
    <button
      class="btn-ghost"
      onclick={() => app.refreshProcesses()}
      disabled={app.loadingProcesses || !isInShell()}
    >
      {app.loadingProcesses ? "Refreshing..." : "Refresh"}
    </button>
  </header>

  {#if !isInShell()}
    <div class="notice">
      Native bridge unavailable — open <code>SyncCommsApp.exe</code> to see live
      audio sessions.
    </div>
  {:else if app.processesError}
    <div class="error">bridge error: {app.processesError}</div>
  {:else}
    <div class="proc-list" role="radiogroup" aria-label="Capture target">
      <button
        type="button"
        role="radio"
        aria-checked={app.settings.captureTarget === null}
        class="proc"
        class:selected={app.settings.captureTarget === null}
        onclick={() => app.setCaptureTarget(null)}
      >
        <span class="proc-main">
          <span class="proc-name">System Audio</span>
          <span class="proc-display">capture everything from the default device</span>
        </span>
        <span class="proc-check" aria-hidden="true">
          {app.settings.captureTarget === null ? "●" : "○"}
        </span>
      </button>

      {#each app.processes as p (p.pid)}
        <button
          type="button"
          role="radio"
          aria-checked={app.settings.captureTarget === p.processName}
          class="proc"
          class:selected={app.settings.captureTarget === p.processName}
          onclick={() => app.setCaptureTarget(p.processName)}
        >
          <span class="proc-main">
            <span class="proc-name">{p.processName}</span>
            {#if p.displayName}
              <span class="proc-display">{p.displayName}</span>
            {/if}
          </span>
          <span class="proc-meta">
            <span class="proc-pid">pid {p.pid}</span>
            <span class="proc-check" aria-hidden="true">
              {app.settings.captureTarget === p.processName ? "●" : "○"}
            </span>
          </span>
        </button>
      {/each}

      {#if app.processes.length === 0 && !app.loadingProcesses}
        <div class="empty">No active audio sessions detected.</div>
      {/if}
    </div>
  {/if}

  {#if app.lastProcessRefresh || app.settings.captureTarget !== null}
    <div class="meta">
      Selected: <strong>{app.settings.captureTarget ?? "System Audio"}</strong>
      {#if app.lastProcessRefresh}
        · Last refresh {app.lastProcessRefresh.toLocaleTimeString()}
        · {app.processes.length} session{app.processes.length === 1 ? "" : "s"}
      {/if}
      {#if app.settingsError}
        · <span class="meta-err">save failed: {app.settingsError}</span>
      {/if}
    </div>
  {/if}
</section>

<section class="card">
  <header class="card-header">
    <div class="card-title-row">
      <div>
        <h3>Microphone</h3>
        <p class="card-sub">
          Include your microphone input in the recording.
        </p>
      </div>
      <label class="switch">
        <input
          type="checkbox"
          checked={app.settings.includeMic}
          onchange={(e) => app.setIncludeMic((e.target as HTMLInputElement).checked)}
        />
        <span class="switch-track"><span class="switch-thumb"></span></span>
      </label>
    </div>
  </header>

  {#if app.settings.includeMic}
    {#if !isInShell()}
      <div class="notice">
        Native bridge unavailable — open <code>SyncCommsApp.exe</code> to see
        microphone devices.
      </div>
    {:else if app.microphonesError}
      <div class="error">bridge error: {app.microphonesError}</div>
    {:else if app.microphones.length === 0 && !app.loadingMicrophones}
      <div class="empty">No active capture devices detected.</div>
    {:else}
      <div class="mic-toolbar">
        <button
          class="btn-ghost"
          onclick={() => app.refreshMicrophones()}
          disabled={app.loadingMicrophones}
        >
          {app.loadingMicrophones ? "Refreshing..." : "Refresh"}
        </button>
      </div>
      <div class="proc-list" role="radiogroup" aria-label="Microphone source">
        <button
          type="button"
          role="radio"
          aria-checked={app.settings.microphoneId === null}
          class="proc"
          class:selected={app.settings.microphoneId === null}
          onclick={() => app.setMicrophoneId(null)}
        >
          <span class="proc-main">
            <span class="proc-name">System Default</span>
            <span class="proc-display">use whichever mic Windows picks for communications</span>
          </span>
          <span class="proc-check" aria-hidden="true">
            {app.settings.microphoneId === null ? "●" : "○"}
          </span>
        </button>

        {#each app.microphones as m (m.deviceId)}
          <button
            type="button"
            role="radio"
            aria-checked={app.settings.microphoneId === m.deviceId}
            class="proc"
            class:selected={app.settings.microphoneId === m.deviceId}
            onclick={() => app.setMicrophoneId(m.deviceId)}
          >
            <span class="proc-main">
              <span class="proc-name">{m.friendlyName || "(unnamed)"}</span>
              {#if m.isDefault}
                <span class="proc-display">system default</span>
              {/if}
            </span>
            <span class="proc-check" aria-hidden="true">
              {app.settings.microphoneId === m.deviceId ? "●" : "○"}
            </span>
          </button>
        {/each}
      </div>
    {/if}
  {/if}
</section>

<style>
  .page-header {
    margin-bottom: 24px;
  }
  h2 {
    margin: 0;
    font-size: 22px;
    font-weight: 600;
    letter-spacing: -0.015em;
  }
  .subtitle {
    margin: 4px 0 0;
    color: var(--muted);
    font-size: 13px;
  }
  .card {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 20px 22px;
    margin-bottom: 16px;
  }
  .card-header {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
    gap: 16px;
    margin-bottom: 16px;
  }
  h3 {
    margin: 0;
    font-size: 14px;
    font-weight: 600;
  }
  .card-sub {
    margin: 4px 0 0;
    color: var(--muted);
    font-size: 12.5px;
    max-width: 480px;
  }
  .btn-ghost {
    background: transparent;
    color: var(--fg-dim);
    border: 1px solid var(--border-strong);
    padding: 6px 12px;
    border-radius: 6px;
    font-weight: 500;
    font-size: 12.5px;
    flex-shrink: 0;
    transition: background 0.12s, border-color 0.12s, color 0.12s;
  }
  .btn-ghost:hover:not(:disabled) {
    background: var(--panel-2);
    color: var(--fg);
    border-color: var(--accent);
  }
  .btn-ghost:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  .proc-list {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .proc {
    background: var(--panel-2);
    color: var(--fg);
    text-align: left;
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 10px 14px;
    width: 100%;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    cursor: pointer;
    transition: border-color 0.12s, background 0.12s, transform 0.06s;
  }
  .proc:hover {
    border-color: var(--border-strong);
    background: #1f2434;
  }
  .proc:active {
    transform: scale(0.998);
  }
  .proc.selected {
    border-color: var(--accent);
    background: linear-gradient(0deg, rgba(110, 231, 183, 0.08), rgba(110, 231, 183, 0.08)),
                var(--panel-2);
  }
  .proc-main {
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-width: 0;
    flex: 1;
  }
  .proc-name {
    font-weight: 500;
    font-size: 13px;
    color: var(--fg);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .proc-display {
    color: var(--muted);
    font-size: 11.5px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .proc-meta {
    display: flex;
    align-items: center;
    gap: 12px;
    flex-shrink: 0;
  }
  .proc-pid {
    color: var(--muted);
    font-family: ui-monospace, "SF Mono", Consolas, monospace;
    font-size: 11.5px;
  }
  .proc-check {
    font-size: 14px;
    line-height: 1;
    color: var(--muted);
    width: 14px;
    text-align: center;
  }
  .proc.selected .proc-check {
    color: var(--accent);
  }

  .empty,
  .notice {
    background: var(--panel-2);
    border: 1px dashed var(--border-strong);
    border-radius: 8px;
    padding: 18px;
    text-align: center;
    color: var(--muted);
    font-size: 13px;
  }
  .error {
    background: var(--danger-bg);
    border: 1px solid var(--danger-border);
    color: var(--danger);
    border-radius: 8px;
    padding: 12px 14px;
    font-family: ui-monospace, monospace;
    font-size: 12.5px;
  }
  .meta {
    margin-top: 14px;
    padding-top: 12px;
    border-top: 1px solid var(--border);
    font-size: 11.5px;
    color: var(--muted);
  }
  .meta strong {
    color: var(--fg-dim);
    font-weight: 500;
  }
  .meta-err {
    color: var(--danger);
  }
  code {
    background: var(--bg);
    padding: 1px 6px;
    border-radius: 3px;
    font-size: 12px;
  }

  /* iOS-style toggle switch */
  .card-title-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 16px;
    flex: 1;
  }
  .switch {
    flex-shrink: 0;
    position: relative;
    display: inline-block;
    width: 38px;
    height: 22px;
    cursor: pointer;
  }
  .switch input {
    position: absolute;
    opacity: 0;
    width: 0;
    height: 0;
  }
  .switch-track {
    position: absolute;
    inset: 0;
    background: var(--border-strong);
    border-radius: 22px;
    transition: background 0.18s ease;
  }
  .switch-thumb {
    position: absolute;
    left: 2px;
    top: 2px;
    width: 18px;
    height: 18px;
    border-radius: 50%;
    background: var(--fg);
    transition: transform 0.18s ease;
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.4);
  }
  .switch input:checked + .switch-track {
    background: var(--accent);
  }
  .switch input:checked + .switch-track .switch-thumb {
    transform: translateX(16px);
    background: var(--accent-text);
  }
  .switch input:focus-visible + .switch-track {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }

  .mic-toolbar {
    display: flex;
    justify-content: flex-end;
    margin-bottom: 10px;
  }
</style>
