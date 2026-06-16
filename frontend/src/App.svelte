<script lang="ts">
  import Sidebar from "./views/Sidebar.svelte";
  import Live from "./views/Live.svelte";
  import Recordings from "./views/Recordings.svelte";
  import Settings from "./views/Settings.svelte";

  type View = "live" | "recordings" | "settings";

  let active = $state<View>("live");
</script>

<div class="layout">
  <Sidebar bind:active />
  <main class="main">
    {#if active === "live"}
      <Live />
    {:else if active === "recordings"}
      <Recordings />
    {:else if active === "settings"}
      <Settings />
    {/if}
  </main>
</div>

<style>
  .layout {
    display: grid;
    grid-template-columns: 200px 1fr;
    height: 100%;
  }
  .main {
    overflow: auto;
    padding: 28px 32px;
  }
</style>
