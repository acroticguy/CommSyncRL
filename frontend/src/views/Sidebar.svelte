<script lang="ts">
  import { app } from "../lib/state.svelte";

  type View = "live" | "recordings" | "settings";

  let { active = $bindable() }: { active: View } = $props();

  const items: { id: View; label: string; hint: string }[] = [
    { id: "live", label: "Live", hint: "Match capture status" },
    { id: "recordings", label: "Recordings", hint: "Browse saved audio" },
    { id: "settings", label: "Settings", hint: "Capture preferences" },
  ];

  const statusLabel = $derived(
    app.connection === "connected"
      ? "Stats API: connected"
      : app.connection === "connecting"
        ? "Stats API: connecting..."
        : "Stats API: not connected"
  );
</script>

<aside class="sidebar">
  <header>
    <h1>SyncComms</h1>
    <span class="version">0.2 · standalone</span>
  </header>
  <nav>
    {#each items as item (item.id)}
      <button
        class="item"
        class:active={active === item.id}
        onclick={() => (active = item.id)}
      >
        <span class="label">{item.label}</span>
        <span class="hint">{item.hint}</span>
      </button>
    {/each}
  </nav>
  <footer>
    <span class="status-dot {app.connection}" aria-hidden="true"></span>
    <span class="status-text">{statusLabel}</span>
  </footer>
</aside>

<style>
  .sidebar {
    background: var(--bg-2);
    border-right: 1px solid var(--border);
    display: flex;
    flex-direction: column;
    padding: 20px 12px 12px;
    gap: 16px;
  }
  header {
    padding: 0 8px 12px;
    border-bottom: 1px solid var(--border);
  }
  h1 {
    margin: 0;
    font-size: 15px;
    font-weight: 600;
    letter-spacing: -0.01em;
    color: var(--fg);
  }
  .version {
    display: block;
    margin-top: 2px;
    font-size: 10.5px;
    color: var(--muted);
    text-transform: uppercase;
    letter-spacing: 0.08em;
  }
  nav {
    display: flex;
    flex-direction: column;
    gap: 2px;
    flex: 1;
  }
  .item {
    background: transparent;
    border: 0;
    color: var(--fg-dim);
    text-align: left;
    padding: 9px 10px;
    border-radius: 6px;
    display: flex;
    flex-direction: column;
    gap: 1px;
    transition: background 0.12s, color 0.12s;
  }
  .item:hover {
    background: var(--panel);
    color: var(--fg);
  }
  .item.active {
    background: var(--panel-2);
    color: var(--fg);
  }
  .label {
    font-size: 13px;
    font-weight: 500;
  }
  .hint {
    font-size: 11px;
    color: var(--muted);
  }
  footer {
    padding: 10px 8px;
    border-top: 1px solid var(--border);
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .status-dot {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: var(--muted);
    flex-shrink: 0;
  }
  .status-dot.connecting {
    background: #fbbf24;
    animation: pulse 1.2s ease-in-out infinite;
  }
  .status-dot.connected {
    background: var(--accent);
    box-shadow: 0 0 6px rgba(110, 231, 183, 0.6);
  }
  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50%      { opacity: 0.35; }
  }
  .status-text {
    font-size: 11px;
    color: var(--muted);
  }
</style>
