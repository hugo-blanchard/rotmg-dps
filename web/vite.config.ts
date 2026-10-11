import { defineConfig } from 'vite'

// GitHub Pages serves the site from /rotmg-dps/, not the domain root.
export default defineConfig({
  base: '/rotmg-dps/',
})
