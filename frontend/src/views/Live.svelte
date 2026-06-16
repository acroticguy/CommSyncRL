<script lang="ts">
  import { app } from "../lib/state.svelte";

  // Highlight rules for the event log: which event names get extra emphasis
  // and what color. Tightly mirrors the Python probe's semantics.
  function eventTone(name: string, data: Record<string, unknown>): "default" | "goal" | "lifecycle" | "replay" {
    if (name === "StatfeedEvent") {
      const t = String(data.Type ?? data.EventName ?? "").toLowerCase();
      if (t === "goal") return "goal";
    }
    if (name === "MatchCreated" || name === "MatchEnded" || name === "CountdownBegin" ||
        name === "MatchPaused" || name === "MatchUnpaused" || name === "MatchDestroyed") {
      return "lifecycle";
    }
    if (name === "ReplayCreated" || name === "ReplayStart" || name === "ReplayEnd") {
      return "replay";
    }
    return "default";
  }

  function shortGuid(g: string | undefined): string {
    if (!g) return "—";
    return g.slice(0, 8) + "…";
  }

  function fmtElapsed(s: number): string {
    if (!isFinite(s)) return "0:00";
    const mins = Math.floor(s / 60);
    const secs = Math.floor(s % 60);
    return `${mins}:${secs.toString().padStart(2, "0")}`;
  }

  function eventSummary(e: { event: string; data: Record<string, unknown> }): string {
    const data = e.data ?? {};
    if (e.event === "StatfeedEvent") {
      const t = data.Type ?? data.EventName ?? "?";
      const main = (data.MainTarget ?? {}) as Record<string, unknown>;
      const sec = (data.SecondaryTarget ?? {}) as Record<string, unknown>;
      const tail = sec.Name ? ` → ${sec.Name}` : "";
      return `${t} · ${main.Name ?? "?"}${tail}`;
    }
    if (e.event === "MatchEnded") {
      return `winner team ${data.WinnerTeamNum ?? "?"}`;
    }
    if (e.event === "BallHit") {
      const ball = (data.Ball ?? {}) as Record<string, unknown>;
      const post = Number(ball.PostHitSpeed ?? 0);
      return `${post.toFixed(0)} kph`;
    }
    return shortGuid(data.MatchGuid as string | undefined);
  }
</script>

<header class="page-header">
  <h2>Live</h2>
  <p class="subtitle">
    Real-time match capture status from the Rocket League Stats API
  </p>
</header>

{#if app.connection !== "connected"}
  <div class="status-banner {app.connection}">
    {#if app.connection === "connecting"}
      <strong>Connecting</strong> to <code>127.0.0.1:49123</code>…
    {:else}
      <strong>Not connected.</strong>
      Make sure Rocket League is running and that
      <code>&lt;RL Install&gt;\TAGame\Config\DefaultStatsAPI.ini</code> has
      <code>PacketSendRate &gt; 0</code>. Restart RL after editing.
      We retry every 2 seconds automatically.
    {/if}
  </div>
{/if}

<div class="capture-banner phase-{app.capture.phase}">
  <div class="capture-headline">
    <span class="capture-dot phase-{app.capture.phase}" aria-hidden="true"></span>
    <span class="capture-phase">
      {#if app.capture.phase === "idle"}Idle
      {:else if app.capture.phase === "armed" && app.capture.segmentCount > 0}Between rounds — waiting for next kickoff
      {:else if app.capture.phase === "armed"}Armed — waiting for kickoff
      {:else if app.capture.phase === "recording"}Recording
      {:else if app.capture.phase === "paused"}Paused
      {:else if app.capture.phase === "finalizing"}Finalizing
      {:else if app.capture.phase === "error"}Error
      {/if}
    </span>
  </div>
  <div class="capture-meta">
    {#if app.capture.phase === "recording"}
      Segment {app.capture.segmentCount + 1} · {app.capture.segmentSeconds.toFixed(1)}s
    {:else if app.capture.phase === "armed" && app.capture.segmentCount > 0}
      {app.capture.segmentCount} segment{app.capture.segmentCount === 1 ? "" : "s"} captured so far
    {:else if app.capture.phase === "armed" || app.capture.phase === "finalizing" || app.capture.phase === "paused"}
      Segment {app.capture.segmentCount}
    {:else if app.capture.phase === "error"}
      {app.capture.lastError ?? "unknown error"}
    {:else if !app.settings.recordingEnabled}
      Recording is OFF in Settings. Match events will still display.
    {/if}
  </div>
</div>

{#if app.playback.phase !== "idle"}
  <div class="capture-banner playback-banner phase-{app.playback.phase}">
    <div class="capture-headline">
      <span class="capture-dot playback-dot phase-{app.playback.phase}" aria-hidden="true"></span>
      <span class="capture-phase">
        {#if app.playback.phase === "searching"}Replay opened — looking for audio
        {:else if app.playback.phase === "noSidecar"}Replay open — no audio recorded for this match
        {:else if app.playback.phase === "playing"}Playing audio synced to replay
        {:else if app.playback.phase === "error"}Playback error
        {/if}
      </span>
      {#if app.playback.phase === "playing"}
        <span
          class="align-tag {app.playback.anchored ? 'anchored' : 'heuristic'}"
          title={app.playback.anchored
            ? `Aligned via ${app.playback.replayPath || '.replay'} goal frames + total length`
            : 'No .replay matched (or parse failed) — using elapsedFloor calibration heuristic'}
        >
          {app.playback.anchored ? "anchored" : "calibrated"}
        </span>
      {/if}
    </div>
    <div class="capture-meta">
      {#if app.playback.phase === "playing"}
        {#if app.playback.activeSegmentIndex >= 0}
          Playing segment {app.playback.activeSegmentIndex + 1} / {app.playback.segmentCount}
        {:else}
          Between segments · {app.playback.segmentCount} total
        {/if}
        · {app.playback.elapsedSec.toFixed(1)}s
      {:else if app.playback.matchGuid}
        match {app.playback.matchGuid.slice(0, 8)}…
      {:else if app.playback.lastError}
        {app.playback.lastError}
      {/if}
    </div>
  </div>
{/if}

<section class="grid">
  <div class="card match-card">
    <h3>Current match</h3>
    {#if app.match}
      <div class="match-row">
        <span class="match-label">Match</span>
        <span class="match-value mono">{shortGuid(app.match.matchGuid)}</span>
      </div>
      <div class="match-row">
        <span class="match-label">Mode</span>
        <span class="match-value">
          {#if app.match.ended}
            ended
          {:else if app.match.paused}
            paused
          {:else if app.match.isReplay}
            replay viewer
          {:else}
            live
          {/if}
        </span>
      </div>
      <div class="match-row">
        <span class="match-label">Elapsed</span>
        <span class="match-value mono">{fmtElapsed(app.match.elapsed)} · frame {app.match.frame}</span>
      </div>
      <div class="match-row">
        <span class="match-label">Goals</span>
        <span class="match-value">{app.match.goals.length}</span>
      </div>

      {#if app.match.goals.length > 0}
        <ul class="goals">
          {#each app.match.goals as g, i (i)}
            <li>
              <span class="goal-time mono">{fmtElapsed(g.elapsed)}</span>
              <span class="goal-by">{g.scorer}</span>
              <span class="goal-team team-{g.team}">team {g.team}</span>
            </li>
          {/each}
        </ul>
      {/if}
    {:else}
      <div class="empty">
        Waiting for a match. Start one in Rocket League and lifecycle events
        will appear here.
      </div>
    {/if}
  </div>

  <div class="card events-card">
    <header class="events-header">
      <h3>Event feed</h3>
      <span class="events-count">{app.events.length} event{app.events.length === 1 ? "" : "s"}</span>
    </header>
    {#if app.events.length === 0}
      <div class="empty">No events yet.</div>
    {:else}
      <ul class="events">
        {#each app.events as e (e.id)}
          {@const tone = eventTone(e.event, e.data)}
          <li class="event tone-{tone}">
            <span class="event-time mono">{e.ts.toLocaleTimeString([], { hour12: false })}</span>
            <span class="event-name">
              {#if tone === "goal"}<span class="goal-tag">GOAL</span>{/if}
              {e.event}
            </span>
            <span class="event-summary">{eventSummary(e)}</span>
          </li>
        {/each}
      </ul>
    {/if}
  </div>
</section>

<style>
  .page-header {
    margin-bottom: 20px;
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

  .status-banner {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 10px 14px;
    margin-bottom: 14px;
    font-size: 13px;
    color: var(--fg-dim);
  }
  .status-banner.connecting {
    border-color: #92571a;
    background: #2a1f0d;
  }
  .status-banner.disconnected {
    border-color: var(--danger-border);
    background: var(--danger-bg);
    color: var(--fg-dim);
  }
  .status-banner code {
    background: var(--bg);
    padding: 1px 6px;
    border-radius: 3px;
    font-size: 12px;
  }

  .capture-banner {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 12px 16px;
    margin-bottom: 14px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 16px;
    transition: border-color 0.18s, background 0.18s;
  }
  .capture-banner.phase-recording {
    border-color: #b91c1c;
    background: linear-gradient(0deg, rgba(220, 38, 38, 0.06), rgba(220, 38, 38, 0.06)),
                var(--panel);
  }
  .capture-banner.phase-paused {
    border-color: #92571a;
    background: #2a1f0d;
  }
  .capture-banner.phase-armed,
  .capture-banner.phase-finalizing {
    border-color: var(--accent);
  }
  .capture-banner.phase-error {
    border-color: var(--danger-border);
    background: var(--danger-bg);
  }
  .capture-headline {
    display: flex;
    align-items: center;
    gap: 10px;
    font-weight: 600;
    font-size: 14px;
  }
  .capture-dot {
    width: 9px;
    height: 9px;
    border-radius: 50%;
    background: var(--muted);
  }
  .capture-dot.phase-recording {
    background: #ef4444;
    box-shadow: 0 0 8px rgba(239, 68, 68, 0.6);
    animation: rec-pulse 1.4s ease-in-out infinite;
  }
  .capture-dot.phase-armed,
  .capture-dot.phase-finalizing {
    background: var(--accent);
  }
  .capture-dot.phase-paused { background: #fbbf24; }
  .capture-dot.phase-error  { background: var(--danger); }
  @keyframes rec-pulse {
    0%, 100% { opacity: 1; transform: scale(1); }
    50%      { opacity: 0.45; transform: scale(0.85); }
  }
  .capture-meta {
    color: var(--muted);
    font-size: 12.5px;
    font-family: ui-monospace, "SF Mono", Consolas, monospace;
  }
  .capture-banner.phase-error .capture-meta {
    color: var(--danger);
    font-family: ui-monospace, monospace;
  }

  .playback-banner.phase-playing {
    border-color: #818cf8;
    background: linear-gradient(0deg, rgba(129, 140, 248, 0.06), rgba(129, 140, 248, 0.06)),
                var(--panel);
  }
  .playback-banner.phase-noSidecar {
    border-color: var(--border-strong);
    background: var(--panel);
  }
  .playback-dot.phase-playing {
    background: #818cf8;
    box-shadow: 0 0 6px rgba(129, 140, 248, 0.6);
    animation: rec-pulse 1.4s ease-in-out infinite;
  }
  .playback-dot.phase-searching { background: #fbbf24; animation: rec-pulse 1.4s ease-in-out infinite; }
  .playback-dot.phase-noSidecar { background: var(--muted); }

  .grid {
    display: grid;
    grid-template-columns: minmax(280px, 1fr) 2fr;
    gap: 14px;
  }

  .card {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 16px 18px;
  }
  h3 {
    margin: 0 0 12px;
    font-size: 12px;
    font-weight: 600;
    color: var(--muted);
    text-transform: uppercase;
    letter-spacing: 0.06em;
  }

  .match-row {
    display: flex;
    justify-content: space-between;
    padding: 5px 0;
    border-bottom: 1px solid var(--border);
    font-size: 13px;
  }
  .match-row:last-of-type { border-bottom: 0; }
  .match-label { color: var(--muted); }
  .match-value { color: var(--fg); }
  .mono {
    font-family: ui-monospace, "SF Mono", Consolas, monospace;
    font-size: 12px;
  }

  .goals {
    list-style: none;
    padding: 0;
    margin: 12px 0 0;
    border-top: 1px solid var(--border);
    padding-top: 10px;
  }
  .goals li {
    display: grid;
    grid-template-columns: 50px 1fr auto;
    gap: 10px;
    padding: 6px 0;
    align-items: center;
    font-size: 12.5px;
  }
  .goal-time { color: var(--muted); }
  .goal-by { color: var(--fg); font-weight: 500; }
  .goal-team {
    font-size: 11px;
    padding: 2px 6px;
    border-radius: 4px;
    background: var(--panel-2);
    color: var(--muted);
  }
  .goal-team.team-0 { color: #93c5fd; background: #1a2540; }
  .goal-team.team-1 { color: #fca5a5; background: #2a1414; }

  .empty {
    color: var(--muted);
    font-size: 13px;
    padding: 14px 0;
    text-align: center;
  }

  .events-card { display: flex; flex-direction: column; min-height: 0; }
  .events-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 6px;
  }
  .events-header h3 { margin: 0; }
  .events-count {
    font-size: 11px;
    color: var(--muted);
  }
  .events {
    list-style: none;
    padding: 0;
    margin: 0;
    overflow: auto;
    max-height: 60vh;
    border-top: 1px solid var(--border);
  }
  .event {
    display: grid;
    grid-template-columns: 70px 150px 1fr;
    gap: 10px;
    padding: 5px 0;
    font-size: 12.5px;
    border-bottom: 1px solid var(--border);
    align-items: center;
  }
  .event:last-child { border-bottom: 0; }
  .event-time { color: var(--muted); }
  .event-name { color: var(--fg-dim); font-weight: 500; }
  .event-summary { color: var(--muted); font-size: 11.5px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

  .event.tone-lifecycle .event-name { color: #93c5fd; }
  .event.tone-replay    .event-name { color: #c4b5fd; }
  .event.tone-goal      .event-name { color: var(--accent); font-weight: 600; }

  .goal-tag {
    background: var(--accent);
    color: var(--accent-text);
    padding: 1px 5px;
    border-radius: 3px;
    font-size: 10px;
    font-weight: 700;
    letter-spacing: 0.04em;
    margin-right: 6px;
    vertical-align: 1px;
  }

  .align-tag {
    margin-left: 8px;
    padding: 2px 7px;
    border-radius: 3px;
    font-size: 10px;
    font-weight: 600;
    letter-spacing: 0.04em;
    text-transform: uppercase;
    cursor: help;
  }
  .align-tag.anchored  { background: #14532d; color: #86efac; }
  .align-tag.heuristic { background: #422006; color: #fcd34d; }
</style>
