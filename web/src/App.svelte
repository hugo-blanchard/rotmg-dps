<script lang="ts">
  import { onMount } from "svelte";
  import { loadDataset, type Dataset } from "./lib/dataset";
  import { Engine } from "./lib/engine";

  let dataset = $state<Dataset | null>(null);
  let engine = $state<Engine | null>(null);
  let error = $state("");

  onMount(async () => {
    try {
      const loaded = await loadDataset();
      engine = await Engine.start(loaded.text);
      dataset = loaded.dataset;
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    }
  });
</script>

<main>
  <h1>rotmg-dps</h1>
  {#if error}
    <p role="alert">{error}</p>
  {:else if !dataset || !engine}
    <p>Loading…</p>
  {:else}
    <p>{dataset.weapons.length} weapons loaded from build {dataset.build.slice(0, 8)}.</p>
  {/if}
</main>

<footer>Unofficial fan tool, not affiliated with DECA. Game assets © DECA Live Operations GmbH.</footer>
