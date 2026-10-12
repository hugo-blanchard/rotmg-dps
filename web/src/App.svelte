<script lang="ts">
  import { onMount } from "svelte";
  import BuildEditor from "./lib/BuildEditor.svelte";
  import LineChart from "./lib/LineChart.svelte";
  import Results from "./lib/Results.svelte";
  import SimulationView from "./lib/SimulationView.svelte";
  import { defaultSimOptions, simFromQuery, simToQuery, type SimOptions } from "./lib/scenario";
  import {
    abilitiesFor,
    enchantmentsFor,
    enchantmentsForSlot,
    loadDataset,
    loadStatus,
    gearFor,
    maxStats,
    otherPieces,
    setsFor,
    usableEnchantments,
    weaponsFor,
    type Dataset,
    type DatasetStatus,
  } from "./lib/dataset";
  import {
    Engine,
    type EvaluateRequest,
    type EvaluateResponse,
    type GearEnchantments,
    type PlayerStatus,
    type Target,
  } from "./lib/engine";
  import { fromQuery, toQuery } from "./lib/link";
  import { loadSprites, type Sprites } from "./lib/sprites";

  let dataset = $state<Dataset | null>(null);
  let dataStatus = $state<DatasetStatus | null>(null);
  let engine = $state<Engine | null>(null);
  let loadError = $state("");
  let sprites = $state<Sprites | null>(null);

  let classId = $state("");
  let weaponName = $state("");
  let abilityName = $state("");
  let simOptions = $state<SimOptions>({ ...defaultSimOptions });
  // A build pinned to compare with in the simulation.
  let pinned = $state<{ request: EvaluateRequest; label: string } | null>(null);
  function pin() {
    const what = [weaponName, abilityName].filter(Boolean).join(" + ");
    pinned = { request: $state.snapshot(request), label: `${what}, ${attack} ATT ${dexterity} DEX` };
  }
  let attack = $state(0);
  let dexterity = $state(0);
  let wisdom = $state(0);
  let status = $state<PlayerStatus>({ weak: false, damaging: false, berserk: false, dazed: false });
  let target = $state<Target>({ defense: 0, armored: false, armorBroken: false, exposed: false, cursed: false });

  let distance = $state(4);
  let radius = $state(0.5);
  // Armor and ring pieces of the gear's sets that you also wear.
  let wearing = $state<number[]>([]);
  // Enchantment ids on the weapon, up to 4, and on the other gear.
  let enchantments = $state<string[]>([]);
  let gearEnchantments = $state<GearEnchantments>({ ring: [], armor: [], ability: [] });
  let hitOverrides = $state<(number | null)[]>([]);

  let response = $state<EvaluateResponse | null>(null);
  const result = $derived(response?.ok ? response : null);
  const defense = $derived(Number(target.defense) || 0);
  const hitDistance = $derived(Math.max(0.5, Number(distance) || 0.5));
  const totalProjectiles = $derived(
    result ? result.breakdown.subattacks.reduce((n, s) => n + s.projectilesPerVolley, 0) : 1,
  );

  const weapon = $derived(dataset?.weapons.find((w) => w.name === weaponName));
  const cls = $derived(dataset?.classes.find((c) => c.id === classId));
  const ability = $derived(dataset?.abilities?.find((a) => a.name === abilityName));
  // Only pieces of a set the chosen weapon or ability is in count.
  const worn = $derived.by(() => {
    if (!dataset) {
      return [];
    }
    const pieces = setsFor(dataset, [weapon, ability]).flatMap((s) => otherPieces(s).flatMap((p) => p.itemTypes));
    const gear = cls ? gearFor(weapon, cls, pieces).flatMap((g) => g.items.map((i) => i.itemType)) : [];
    return wearing.filter((t) => pieces.includes(t) || gear.includes(t));
  });
  // Only enchantments that can be on the current items together are sent.
  const enchanted = $derived(dataset ? usableEnchantments(enchantments, enchantmentsFor(dataset, weapon)) : []);
  const gearEnchanted = $derived<GearEnchantments>({
    ring: dataset ? usableEnchantments(gearEnchantments.ring, enchantmentsForSlot(dataset, "RING")) : [],
    armor: dataset ? usableEnchantments(gearEnchantments.armor, enchantmentsForSlot(dataset, "ARMOR")) : [],
    // Ability enchantments only mean something with an ability.
    ability:
      dataset && abilityName ? usableEnchantments(gearEnchantments.ability, enchantmentsForSlot(dataset, "ABILITY")) : [],
  });

  // What evaluating and simulating share; evaluating adds the two charts.
  const request = $derived<EvaluateRequest>({
    weapon: weaponName,
    ability: abilityName || undefined,
    attack: Number(attack) || 0,
    dexterity: Number(dexterity) || 0,
    wisdom: Number(wisdom) || 0,
    // Stats the editor doesn't show are at the class's max.
    ...(cls ? maxStats(cls) : {}),
    status: { ...status },
    target: { ...target, defense },
    wearing: [...worn],
    enchantments: [...enchanted],
    gearEnchantments: {
      ring: [...gearEnchanted.ring],
      armor: [...gearEnchanted.armor],
      ability: [...gearEnchanted.ability],
    },
    class: classId,
    hitsPerVolley: [...hitOverrides],
    hitModel: { distance: hitDistance, radius: Math.max(0.05, Number(radius) || 0.5) },
  });

  // Drop what can't be on the items from the editor too, so its selects
  // never hold an id they can't show.
  $effect(() => {
    if (!dataset) {
      return;
    }
    if (enchanted.join() !== enchantments.join()) {
      enchantments = [...enchanted];
    }
    if (JSON.stringify(gearEnchanted) !== JSON.stringify(gearEnchantments)) {
      gearEnchantments = { ...gearEnchanted };
    }
  });

  // Hits typed in for one weapon don't carry over to the next, nor past an
  // enchantment that can add or swap subattacks.
  $effect(() => {
    void weaponName;
    void enchanted.join();
    hitOverrides = [];
  });
  let latest = 0;

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
      const linked = fromQuery(location.search, d);
      simOptions = simFromQuery(location.search);
      if (linked) {
        ({
          classId,
          weaponName,
          abilityName,
          attack,
          dexterity,
          wisdom,
          status,
          target,
          distance,
          radius,
          wearing,
          enchantments,
          gearEnchantments,
        } = linked);
      } else {
        const start = d.classes.find((c) => c.id === "Wizard") ?? d.classes[0];
        classId = start.id;
        weaponName = weaponsFor(d, start)[0]?.name ?? "";
        abilityName = abilitiesFor(d, start)[0]?.name ?? "";
        attack = start.attack.max;
        dexterity = start.dexterity.max;
        wisdom = start.wisdom?.max ?? 0;
      }
      dataset = d;
    } catch (e) {
      loadError = e instanceof Error ? e.message : String(e);
    }
  });

  // Keep the URL on the current build, so it can be shared.
  $effect(() => {
    if (!dataset || !weaponName) {
      return;
    }
    const query = toQuery({
      classId,
      weaponName,
      abilityName,
      attack,
      dexterity,
      wisdom,
      status,
      target,
      distance,
      radius,
      wearing: worn,
      enchantments: enchanted,
      gearEnchantments: gearEnchanted,
    });
    const q = new URLSearchParams(query);
    simToQuery(simOptions, q);
    history.replaceState(null, "", `${location.pathname}?${q}`);
  });

  $effect(() => {
    if (!engine || !weaponName) {
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
  <h1>rotmg-dps</h1>
  <p>Damage per second for any Realm of the Mad God weapon, against any target.</p>
</header>

<main>
  {#if loadError}
    <p role="alert">{loadError}</p>
  {:else if !dataset}
    <p>Loading…</p>
  {:else}
    <BuildEditor
      {dataset}
      wornSets={result?.sets ?? []}
      enchantmentStats={result?.enchantmentStats ?? []}
      bind:classId
      bind:weaponName
      bind:abilityName
      bind:attack
      bind:dexterity
      bind:wisdom
      bind:status
      bind:target
      bind:distance
      bind:radius
      bind:wearing
      bind:enchantments
      bind:gearEnchantments
    />
    {#if result}
      <Results
        breakdown={result.breakdown}
        build={result.build}
        ability={result.ability}
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
      {#if engine && weapon}
        <div class="sim-row">
          <SimulationView
            {engine}
            {request}
            {weapon}
            {ability}
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
            onpin={pin}
            onunpin={() => (pinned = null)}
          />
        </div>
      {/if}
    {:else if response && !response.ok}
      <p role="alert">{response.error}</p>
    {/if}
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
  .sim-row {
    grid-column: 2;
    min-width: 0;
  }
  @media (max-width: 760px) {
    .sim-row {
      grid-column: 1;
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
