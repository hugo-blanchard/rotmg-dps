<script lang="ts">
  import { onMount } from "svelte";
  import BuildEditor from "./lib/BuildEditor.svelte";
  import LineChart from "./lib/LineChart.svelte";
  import Results from "./lib/Results.svelte";
  import SimulationView from "./lib/SimulationView.svelte";
  import { defaultSimOptions, simFromQuery, simToQuery, type SimOptions } from "./lib/scenario";
  import {
    enchantmentSlotsOf,
    enchantmentsForItem,
    suggested,
    itemsFor,
    loadDataset,
    loadStatus,
    maxStats,
    slots,
    usableSlots,
    type Dataset,
    type DatasetStatus,
    type Slot,
  } from "./lib/dataset";
  import { Engine, type EvaluateRequest, type EvaluateResponse, type PlayerStatus, type Target } from "./lib/engine";
  import { fromQuery, toQuery } from "./lib/link";
  import { loadSprites, type Sprites } from "./lib/sprites";

  let dataset = $state<Dataset | null>(null);
  let dataStatus = $state<DatasetStatus | null>(null);
  let engine = $state<Engine | null>(null);
  let loadError = $state("");
  let sprites = $state<Sprites | null>(null);

  // Advanced shows more of the numbers and the simulation; the build is the
  // same in both.
  let advanced = $state(false);
  let tab = $state<"numbers" | "simulation">("numbers");

  let classId = $state("");
  let items = $state<Record<Slot, string>>({ weapon: "", ability: "", armor: "", ring: "" });
  const noEnchants = () => ["", "", "", ""];
  let enchants = $state<Record<Slot, string[]>>({
    weapon: noEnchants(),
    ability: noEnchants(),
    armor: noEnchants(),
    ring: noEnchants(),
  });
  let status = $state<PlayerStatus>({ weak: false, damaging: false, berserk: false, dazed: false });
  let target = $state<Target>({ defense: 0, armored: false, armorBroken: false, exposed: false, cursed: false });
  let distance = $state(4);
  let radius = $state(0.5);
  let hitOverrides = $state<(number | null)[]>([]);

  let simOptions = $state<SimOptions>({ ...defaultSimOptions });
  // A build pinned to compare with in the simulation.
  let pinned = $state<{ request: EvaluateRequest; label: string } | null>(null);
  function pin() {
    pinned = { request: $state.snapshot(request), label: slots.map((s) => items[s]).filter(Boolean).join(", ") };
  }

  let response = $state<EvaluateResponse | null>(null);
  const result = $derived(response?.ok ? response : null);
  const defense = $derived(Number(target.defense) || 0);
  const hitDistance = $derived(Math.max(0.5, Number(distance) || 0.5));
  const totalProjectiles = $derived(
    result ? result.breakdown.subattacks.reduce((n, s) => n + s.projectilesPerVolley, 0) : 1,
  );

  const cls = $derived(dataset?.classes.find((c) => c.id === classId));
  const weapon = $derived(dataset?.weapons.find((w) => w.name === items.weapon));
  const ability = $derived(dataset?.abilities?.find((a) => a.name === items.ability));

  // A class change keeps the items the new class can use and fills the
  // rest with its first choice players can get that isn't a reskin; an
  // empty ability, armor or ring stays empty.
  $effect(() => {
    if (!dataset || !cls) return;
    const fixed = { ...items };
    for (const slot of slots) {
      const choices = itemsFor(dataset, cls, slot);
      if (!choices.some((i) => i.name === items[slot]) && (items[slot] || slot === "weapon")) {
        fixed[slot] = (choices.find(suggested) ?? choices[0])?.name ?? "";
      }
    }
    if (slots.some((s) => fixed[s] !== items[s])) {
      items = fixed;
    }
  });

  // Enchantments the item in their slot can't hold are dropped, so nothing
  // shows that isn't sent.
  $effect(() => {
    if (!dataset || !cls) return;
    const fixed = { ...enchants };
    for (const slot of slots) {
      const item = itemsFor(dataset, cls, slot).find((i) => i.name === items[slot]);
      fixed[slot] = usableSlots(
        enchants[slot],
        enchantmentsForItem(dataset, slot, item).map((o) => o.enchantment),
        enchantmentSlotsOf(dataset, item),
      );
    }
    if (slots.some((s) => fixed[s].join() !== enchants[s].join())) {
      enchants = fixed;
    }
  });

  // What evaluating and simulating share; evaluating adds the two charts.
  // Builds are always for a maxed character.
  const request = $derived<EvaluateRequest>({
    weapon: items.weapon,
    ability: items.ability || undefined,
    armor: items.armor || undefined,
    ring: items.ring || undefined,
    ...(cls ? maxStats(cls) : { attack: 0, dexterity: 0 }),
    status: { ...status },
    target: { ...target, defense },
    enchantments: enchants.weapon.filter(Boolean),
    gearEnchantments: {
      ring: enchants.ring.filter(Boolean),
      armor: enchants.armor.filter(Boolean),
      ability: items.ability ? enchants.ability.filter(Boolean) : [],
    },
    class: classId,
    hitsPerVolley: [...hitOverrides],
    hitModel: { distance: hitDistance, radius: Math.max(0.05, Number(radius) || 0.5) },
  });

  // Hits typed in for one weapon don't carry over to the next, nor past an
  // enchantment that can add or swap subattacks.
  $effect(() => {
    void items.weapon;
    void enchants.weapon.join();
    hitOverrides = [];
  });
  let latest = 0;

  // On a phone the numbers sit below the whole build, so a bar at the
  // bottom carries the DPS while they're out of sight.
  let resultsEl = $state<HTMLElement | null>(null);
  let resultsInView = $state(true);
  // Checked again when the page changes height, as when sprites load.
  $effect(() => {
    const el = resultsEl;
    if (!el) return;
    const check = () => {
      const r = el.getBoundingClientRect();
      resultsInView = r.top < innerHeight && r.bottom > 0;
    };
    check();
    const resized = new ResizeObserver(check);
    resized.observe(document.body);
    addEventListener("scroll", check, { passive: true });
    addEventListener("resize", check);
    return () => {
      resized.disconnect();
      removeEventListener("scroll", check);
      removeEventListener("resize", check);
    };
  });

  const whole = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });

  // "3 days ago (Oct 8, 2026)", from the status.json of the deployed dataset.
  const extracted = $derived.by(() => {
    if (!dataStatus || dataStatus.build !== dataset?.build) {
      return "";
    }
    const at = new Date(dataStatus.extractedAt);
    if (Number.isNaN(at.getTime())) {
      return "";
    }
    const days = Math.floor((Date.now() - at.getTime()) / 86_400_000);
    const ago = days <= 0 ? "today" : days === 1 ? "yesterday" : `${days} days ago`;
    return `${ago} (${at.toLocaleDateString("en", { day: "numeric", month: "short", year: "numeric" })})`;
  });

  onMount(async () => {
    loadSprites().then((s) => (sprites = s));
    loadStatus().then((s) => (dataStatus = s));
    try {
      const loaded = await loadDataset();
      engine = await Engine.start(loaded.text);
      const d = loaded.dataset;
      const query = new URLSearchParams(location.search);
      advanced = query.get("view") === "advanced";
      tab = query.get("tab") === "simulation" ? "simulation" : "numbers";
      simOptions = simFromQuery(location.search);
      const linked = fromQuery(location.search, d);
      if (linked) {
        ({ classId, items, enchants, status, target, distance, radius } = linked);
      } else {
        const start = d.classes.find((c) => c.id === "Wizard") ?? d.classes[0];
        classId = start.id;
        items = Object.fromEntries(
          slots.map((s) => {
            const choices = itemsFor(d, start, s);
            return [s, (choices.find(suggested) ?? choices[0])?.name ?? ""];
          }),
        ) as Record<Slot, string>;
      }
      dataset = d;
    } catch (e) {
      loadError = e instanceof Error ? e.message : String(e);
    }
  });

  // Keep the URL on the current build and view, so it can be shared.
  $effect(() => {
    if (!dataset || !items.weapon) {
      return;
    }
    const q = new URLSearchParams(toQuery({ classId, items, enchants, status, target, distance, radius }));
    simToQuery(simOptions, q);
    if (advanced) q.set("view", "advanced");
    if (tab === "simulation") q.set("tab", "simulation");
    history.replaceState(null, "", `${location.pathname}?${q}`);
  });

  $effect(() => {
    if (!engine || !items.weapon) {
      return;
    }
    const withCharts = {
      ...request,
      hitCurve: { from: 0.5, to: Math.max(10, Math.ceil(hitDistance)), step: 0.25 },
      // The chart covers 0-150 DEF, or further if the target's DEF is higher.
      curve: { from: 0, to: Math.max(150, Math.ceil(defense / 10) * 10), step: 1 },
    };
    // Only the newest answer is shown, however the worker's replies interleave.
    const id = ++latest;
    engine.evaluate(withCharts).then((r) => {
      if (id === latest) {
        response = r;
      }
    });
  });
</script>

<header>
  <div>
    <h1>rotmg-dps</h1>
    <p>Damage per second for any Realm of the Mad God build, against any target.</p>
  </div>
  <div class="mode" role="group" aria-label="How much to show">
    <button type="button" aria-pressed={!advanced} onclick={() => (advanced = false)}>Simple</button>
    <button type="button" aria-pressed={advanced} onclick={() => (advanced = true)}>Advanced</button>
  </div>
</header>

{#if result && !resultsInView}
  <button type="button" class="peek" onclick={() => resultsEl?.scrollIntoView({ behavior: "smooth" })}>
    <strong>{whole.format(result.build.dps)} DPS</strong> See the numbers
  </button>
{/if}

<main>
  {#if loadError}
    <p role="alert">{loadError}</p>
  {:else if !dataset}
    <p>Loading…</p>
  {:else}
    <BuildEditor
      {dataset}
      {sprites}
      {advanced}
      {engine}
      {request}
      wornSets={result?.sets ?? []}
      addedStats={[...(result?.enchantmentStats ?? []), ...(result?.itemStats ?? [])]}
      bind:classId
      bind:items
      bind:enchants
      bind:status
      bind:target
      bind:distance
      bind:radius
    />
    <div class="results" bind:this={resultsEl}>
      <div class="tabs" role="tablist" aria-label="Results">
        <button
          type="button"
          role="tab"
          id="tab-numbers"
          aria-selected={tab === "numbers"}
          aria-controls="panel"
          onclick={() => (tab = "numbers")}>Numbers</button
        >
        <button
          type="button"
          role="tab"
          id="tab-simulation"
          aria-selected={tab === "simulation"}
          aria-controls="panel"
          onclick={() => (tab = "simulation")}>Simulation</button
        >
      </div>
      <div id="panel" role="tabpanel" aria-labelledby="tab-{tab}">
        {#if !result}
          {#if response && !response.ok}
            <p role="alert">{response.error}</p>
          {/if}
        {:else if tab === "numbers"}
          <Results
            breakdown={result.breakdown}
            build={result.build}
            ability={result.ability}
            {advanced}
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
              {#if advanced && result.hitModelGaps?.length}
                <p class="note">
                  The hit estimate can't follow {result.hitModelGaps.join(" or ")}, so it assumes every shot hits.
                </p>
              {:else if advanced && result.hitCurve}
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
        {:else if engine && weapon}
          <SimulationView
            {engine}
            {request}
            {weapon}
            {ability}
            {advanced}
            className={classId}
            {sprites}
            expectedDps={result.breakdown.subattacks.reduce((n, s) => n + s.dps, 0)}
            expectedHits={result.breakdown.subattacks.reduce((n, s) => n + s.hitsPerVolley, 0)}
            expectedAbilityDps={result.ability?.dps}
            expectedProcDps={result.breakdown.procs.reduce((n, p) => n + p.dps, 0)}
            expectedTotalDps={result.build?.dps ?? result.breakdown.dps}
            bosses={dataset?.targets ?? []}
            bind:options={simOptions}
            {pinned}
            expectedUptimes={result.build?.uptimes ?? []}
            unmodelled={result.breakdown.unsupported.length + (result.ability?.unsupported.length ?? 0) > 0}
            onpin={pin}
            onunpin={() => (pinned = null)}
          />
        {/if}
      </div>
    </div>
  {/if}
</main>

<footer>
  <p>Unofficial fan tool, not affiliated with DECA. Game assets © DECA Live Operations GmbH.</p>
  {#if dataset}
    <p>
      Game data from client build {dataset.build.slice(0, 8)}{#if extracted}, extracted {extracted}{/if}.
    </p>
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
    display: flex;
    flex-wrap: wrap;
    gap: 0.75rem 1.5rem;
    align-items: center;
    justify-content: space-between;
  }
  h1 {
    margin: 0;
    font-size: 1.5rem;
  }
  header p {
    margin: 0.25rem 0 0;
    color: var(--text-2);
  }
  .mode {
    display: inline-flex;
    border: 1px solid var(--border);
    border-radius: 8px;
    overflow: hidden;
  }
  .mode button,
  .tabs button {
    font: inherit;
    padding: 0.35rem 0.9rem;
    border: none;
    background: var(--page);
    color: var(--text-2);
    cursor: pointer;
  }
  .mode button[aria-pressed="true"] {
    background: var(--accent);
    color: white;
  }
  main {
    margin-top: 1.5rem;
    display: grid;
    grid-template-columns: minmax(0, 24rem) minmax(0, 1fr);
    gap: 2rem;
    align-items: start;
  }
  @media (max-width: 760px) {
    main {
      grid-template-columns: minmax(0, 1fr);
    }
  }
  .results {
    display: grid;
    gap: 1rem;
    min-width: 0;
  }
  .tabs {
    display: flex;
    gap: 0.25rem;
    border-bottom: 1px solid var(--border);
  }
  .tabs button {
    background: none;
    border-bottom: 2px solid transparent;
    margin-bottom: -1px;
    font-weight: 600;
  }
  .tabs button[aria-selected="true"] {
    color: var(--text);
    border-bottom-color: var(--accent);
  }
  .note {
    margin: 0;
    font-size: 0.9rem;
    color: var(--text-2);
  }
  .peek {
    display: none;
  }
  @media (max-width: 760px) {
    .peek {
      display: flex;
      gap: 0.6rem;
      align-items: baseline;
      position: fixed;
      left: 50%;
      bottom: 0.75rem;
      transform: translateX(-50%);
      z-index: 10;
      padding: 0.5rem 1rem;
      border: 1px solid var(--accent);
      border-radius: 999px;
      background: var(--surface);
      color: var(--text-2);
      font: inherit;
      font-size: 0.9rem;
      box-shadow: 0 2px 10px rgb(0 0 0 / 0.35);
      cursor: pointer;
      white-space: nowrap;
    }
    .peek strong {
      color: var(--text);
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
