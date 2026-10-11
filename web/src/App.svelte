<script lang="ts">
  import { onMount } from "svelte";
  import BuildEditor from "./lib/BuildEditor.svelte";
  import LineChart from "./lib/LineChart.svelte";
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

  let distance = $state(4);
  let radius = $state(0.5);
  let hitOverrides = $state<(number | null)[]>([]);

  let response = $state<EvaluateResponse | null>(null);
  const result = $derived(response?.ok ? response : null);
  const defense = $derived(Number(target.defense) || 0);
  const hitDistance = $derived(Math.max(0.5, Number(distance) || 0.5));
  const totalProjectiles = $derived(
    result ? result.breakdown.subattacks.reduce((n, s) => n + s.projectilesPerVolley, 0) : 1,
  );

  // Hits typed in for one weapon don't carry over to the next.
  $effect(() => {
    void weaponName;
    hitOverrides = [];
  });
  let latest = 0;

  const whole = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });

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
      hitsPerVolley: [...hitOverrides],
      hitModel: { distance: hitDistance, radius: Math.max(0.05, Number(radius) || 0.5) },
      hitCurve: { from: 0.5, to: Math.max(10, Math.ceil(hitDistance)), step: 0.25 },
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
    <BuildEditor {dataset} bind:classId bind:weaponName bind:attack bind:dexterity bind:status bind:target bind:distance bind:radius />
    {#if result}
      <Results
        breakdown={result.breakdown}
        {defense}
        distance={hitDistance}
        {hitOverrides}
        onhits={(i, hits) => {
          const next = [...hitOverrides];
          next[i] = hits;
          hitOverrides = next;
        }}
      >
        {#snippet chart()}
          {#if result.curve}
            <LineChart
              caption="DPS against target DEF"
              points={result.curve.map((p) => ({ x: p.defense, y: p.dps }))}
              selected={defense}
              onpick={(d) => (target.defense = d)}
              formatX={(d) => `${d} DEF`}
              formatY={(v) => `${whole.format(v)} DPS`}
              sliderLabel="Target DEF on the DPS chart"
              keyStep={1}
              tableEvery={10}
            />
          {/if}
          {#if result.hitModelGaps?.length}
            <p class="note">
              The hit estimate can't follow {result.hitModelGaps.join(" or ")}, so it assumes every shot hits.
            </p>
          {:else if result.hitCurve}
            <LineChart
              caption="Estimated hits per volley against distance"
              points={result.hitCurve.map((p) => ({ x: p.distance, y: p.hits }))}
              selected={hitDistance}
              onpick={(d) => (distance = d)}
              formatX={(d) => `${d} tiles`}
              formatY={(h) => `${h} of ${totalProjectiles} hits`}
              sliderLabel="Distance to the target on the hits chart"
              keyStep={0.25}
              stepped
              integerY
              yAtLeast={totalProjectiles}
              tableEvery={1}
            />
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
  .note {
    margin: 0;
    font-size: 0.9rem;
    color: var(--text-2);
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
