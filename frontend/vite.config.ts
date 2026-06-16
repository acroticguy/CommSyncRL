import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";
import { viteSingleFile } from "vite-plugin-singlefile";

// We deliver the production build as a single self-contained index.html (all
// JS and CSS inlined). The C++ shell reads this file off disk and hands it
// to WebView2 via NavigateToString — no external resource loads, no `file://`
// CORS issues, no need for virtual host mapping or an embedded HTTP server.
//
// In dev (`npm run dev`), Vite's normal dev server is used; the singlefile
// plugin only kicks in for `vite build`.

export default defineConfig({
  plugins: [svelte(), viteSingleFile()],
  server: {
    port: 5173,
    strictPort: true,
  },
  build: {
    outDir: "dist",
    emptyOutDir: true,
    target: "esnext",
    sourcemap: false, // singlefile + sourcemaps don't combine cleanly; omit
  },
});
