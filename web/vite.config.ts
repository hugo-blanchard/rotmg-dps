import { svelte } from "@sveltejs/vite-plugin-svelte";
import { defineConfig } from "vite";

// The engine's threads share memory, which browsers only allow on a page
// that's cross-origin isolated. Cloudflare sends these headers from
// public/_headers; the dev and preview servers send them here.
const isolation = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
};

export default defineConfig({
  plugins: [svelte()],
  server: { headers: isolation },
  preview: { headers: isolation },
});
