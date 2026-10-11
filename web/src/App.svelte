<script lang="ts">
  import { onMount } from "svelte";
  import BuildEditor from "./lib/BuildEditor.svelte";
  import DpsChart from "./lib/DpsChart.svelte";
  import Results from "./lib/Results.svelte";
  import { loadDataset, weaponsFor, type Dataset } from "./lib/dataset";
  import { Engine, type EvaluateResponse, type PlayerStatus, type Target } from "./lib/engine";

  let dataset = $state<Dataset | null>(null);
  let engine = $state<Engine | null>(null);
  let loadError = $state("");

  let classId = $state("");
  let weaponName = $state("");
  let attack = $state(0);
  let dexterity = $state(0);
  let status = $state<PlayerStatus>({ weak: false, damaging: false, berserk: false, dazed: false });
  let target = $state<Target>({ defense: 0, armored: false, armorBroken: false, exposed: false, cursed: false });

  let response = $state<EvaluateResponse | null>(null);
  const result = $derived(response?.ok ? response : null);
  const defense = $derived(Number(target.defense) || 0);
  let latest = 0;

  onMount(async () => {
    try {
      const loaded = await loadDataset();
      engine = await Engine.start(loaded.text);
      const d = loaded.dataset;
      const start = d.classes.find((c) => c.id === "Wizard") ?? d.classes[0];
      classId = start.id;
      weaponName = weaponsFor(d, start)[0]?.name ?? "";
      attack = start.attack.max;
      dexterity = start.dexterity.max;
      dataset = d;
    } catch (e) {
      loadError = e instanceof Error ? e.message : String(e);
    }
  });

  $effect(() => {
    if (!engine || !weaponName) {
      return;
    }
    const request = {
      weapon: weaponName,
      attack: Number(attack) || 0,
      dexterity: Number(dexterity) || 0,
      status: { ...status },
      target: { ...target, defense },
      // The chart covers 0-150 DEF, or further if the target's DEF is higher.
      curve: { from: 0, to: Math.max(150, Math.ceil(defense / 10) * 10), step: 1 },
    };
    // Only the newest answer is shown, however the worker's replies interleave.
    const id = ++latest;
    engine.evaluate(request).then((r) => {
      if (id === latest) {
        response = r;
      }
    });
  });
</script>

<header>
  <h1>rotmg-dps</h1>
  <p>Damage per second for any Realm of the Mad God weapon, against any target.</p>
</header>

<main>
  {#if loadError}
    <p role="alert">{loadError}</p>
  {:else if !dataset}
    <p>Loading…</p>
  {:else}
    <BuildEditor {dataset} bind:classId bind:weaponName bind:attack bind:dexterity bind:status bind:target />
    {#if result}
      <Results breakdown={result.breakdown} {defense}>
        {#snippet chart()}
          {#if result.curve}
            <DpsChart curve={result.curve} {defense} onpick={(d) => (target.defense = d)} />
          {/if}
        {/snippet}
      </Results>
    {:else if response && !response.ok}
      <p role="alert">{response.error}</p>
    {/if}
  {/if}
</main>

<footer>
  <p>Unofficial fan tool, not affiliated with DECA. Game assets © DECA Live Operations GmbH.</p>
  {#if dataset}
    <p>Game data from client build {dataset.build.slice(0, 8)}.</p>
  {/if}
</footer>

<style>
  header,
  main,
  footer {
    max-width: 72rem;
    margin: 0 auto;
    padding: 0 1rem;
  }
  header {
    padding-top: 1.5rem;
  }
  h1 {
    margin: 0;
    font-size: 1.5rem;
  }
  header p {
    margin: 0.25rem 0 1.5rem;
    color: var(--text-2);
  }
  main {
    display: grid;
    grid-template-columns: minmax(0, 22rem) minmax(0, 1fr);
    gap: 2rem;
    align-items: start;
  }
  @media (max-width: 760px) {
    main {
      grid-template-columns: minmax(0, 1fr);
    }
  }
  footer {
    margin-top: 3rem;
    padding-bottom: 2rem;
    font-size: 0.85rem;
    color: var(--muted);
  }
  footer p {
    margin: 0.2rem 0;
  }
</style>
