<script lang="ts">
  // Asks the engine for the build with the most DPS against the current
  // target, and lets you take one of the builds it finds, with an undo. The
  // exact bounded search proves the best armor, ring and enchantments for
  // your weapon and ability among armors and rings you list; the wide search
  // tries every item but can miss a build.
  import { tick } from "svelte";
  import Picker from "./Picker.svelte";
  import Sprite from "./Sprite.svelte";
  import { itemEffects, itemsFor, slots, statSummary, type Dataset, type DatasetClass, type Slot } from "./dataset";
  import type {
    Engine,
    EstimateResponse,
    ExplainResponse,
    EvaluateRequest,
    OptimizeOptions,
    OptimizeProgress,
    OptimizeResponse,
    OptimizedBuild,
    BurstWindow,
    DpsWeights,
  } from "./engine";
  import { isBurstWindow } from "./engine";
  import { itemFilters, itemOptions } from "./itemOptions";
  import { itemIcon, type Sprites } from "./sprites";

  let {
    active,
    engine,
    request,
    dataset,
    cls,
    sprites,
    items = $bindable(),
    enchants = $bindable(),
    burst = $bindable(),
    castWeights = $bindable(),
  }: {
    /** Whether its tab is showing: it only estimates then. */
    active: boolean;
    engine: Engine | null;
    /** The current build, which the search starts from. */
    request: EvaluateRequest;
    dataset: Dataset;
    cls: DatasetClass;
    sprites: Sprites | null;
    items: Record<Slot, string>;
    enchants: Record<Slot, string[]>;
    /** The burst window, shared with the numbers. */
    burst: BurstWindow;
    /** How the page times casts, set to the search's weights when one of its builds is taken. */
    castWeights: DpsWeights | undefined;
  } = $props();

  const slotNames: Record<Slot, string> = { weapon: "Weapon", ability: "Ability", armor: "Armor", ring: "Ring" };
  let mode = $state<"exact" | "wide">("exact");
  // The armors and rings the exact search chooses from, by name ("" for
  // none): the build's own until you change the list.
  let armors = $state<string[] | null>(null);
  let rings = $state<string[] | null>(null);
  const armorList = $derived(armors ?? [items.armor]);
  const ringList = $derived(rings ?? [items.ring]);
  // Which list the picker adds to.
  let adding = $state<"armor" | "ring">("armor");
  let picking = $state(false);
  // How the search whose builds are shown was run.
  let searchedMode = $state<"exact" | "wide">("exact");
  let lock = $state<Record<Slot, boolean>>({ weapon: false, ability: false, armor: false, ring: false });
  let pools = $state({ tiered: true, untiered: true, set: true });
  let enchantMode = $state<OptimizeOptions["enchantments"]>("best");
  let artifacts = $state(true);
  let perItem = $state(2);
  let around = $state("every stat");
  // From 0, the weapon only, through 0.5, both in full, to 1, the ability only.
  let balance = $state(0.5);
  let goal = $state<"sustained" | "burst">("sustained");
  // How the search whose builds are shown ranked them: around a stat, by the
  // damage that stat adds.
  let rankedBy = $state<{ balance: number; burst: boolean; stat?: string }>({ balance: 0.5, burst: false });
  let searching = $state(false);
  let found = $state<OptimizeResponse | null>(null);
  // The running search's last report, and whether the builds shown are
  // from a search that was stopped.
  let progress = $state<OptimizeProgress | null>(null);
  let stopped = $state(false);
  let startedAt = 0;
  let now = $state(0);
  let estimate = $state<EstimateResponse | null>(null);
  // The result whose details are open, and its explanation.
  let opened = $state<number | null>(null);
  let explained = $state<ExplainResponse | null>(null);
  // The options of the search whose builds are shown, to explain them by.
  let searched: OptimizeOptions | null = null;
  const enchantmentName = (id: string) => (dataset.enchantments ?? []).find((e) => e.id === id)?.name ?? id;
  // The build before the last one taken, for undo.
  let previous = $state<{
    items: Record<Slot, string>;
    enchants: Record<Slot, string[]>;
    castWeights: DpsWeights | undefined;
  } | null>(null);

  const whole = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });
  const usableSlots = $derived(slots.filter((s) => itemsFor(dataset, cls, s).length > 0));
  const stats: [string, string][] = [
    ["ATT", "Attack"],
    ["DEX", "Dexterity"],
    ["SPD", "Speed"],
    ["WIS", "Wisdom"],
    ["VIT", "Vitality"],
    ["DEF", "Defense"],
    ["MAXHP", "Max HP"],
    ["MAXMP", "Max MP"],
  ];

  // The stats something the class can use deals damage that scales with,
  // the only ones worth building around.
  let classStats = $state<string[]>([]);
  $effect(() => {
    const id = cls.id;
    engine?.damageStats(id).then((found) => {
      if (cls.id !== id) return;
      classStats = found;
      if (stats.some(([s]) => s === around) && !found.includes(around)) around = "every stat";
    });
  });
  const statName = (id: string) => stats.find(([s]) => s === id)?.[1] ?? id;
  const aroundStat = $derived(mode === "wide" && stats.some(([s]) => s === around) ? around : undefined);

  const weights = (t: number) => ({ weapon: Math.min(1, 2 * (1 - t)), ability: Math.min(1, 2 * t) });
  const balances = [0, 0.25, 0.5, 0.75, 1];
  const balanceText: Record<number, string> = {
    0: "Weapon only",
    0.25: "Weapon, plus half the ability",
    0.5: "Weapon and ability",
    0.75: "Ability, plus half the weapon",
    1: "Ability only",
  };
  const castText: Record<number, string> = {
    0: "the weapon's DPS",
    0.25: "the weapon's DPS plus half the ability's",
    0.75: "the ability's DPS plus half the weapon's",
    1: "the ability's DPS",
  };
  // What a gain on the start's score is in, for the search shown.
  const scoreUnit = $derived.by(() => {
    const what = rankedBy.burst ? "damage" : "DPS";
    if (rankedBy.stat) return ` ${what} from ${rankedBy.stat}`;
    const units: Record<number, string> = { 0: ` weapon ${what}`, 0.5: rankedBy.burst ? " damage" : "", 1: ` ability ${what}` };
    return units[rankedBy.balance] ?? " weighted";
  });
  const burstValid = $derived(isBurstWindow(burst));

  const asResponse = (p: OptimizeProgress | null): OptimizeResponse | null =>
    p && { ok: true, baseline: p.baseline, evaluated: p.evaluated, builds: p.builds };

  const hasSlot = (slot: Slot) => usableSlots.includes(slot);
  const canExact = $derived(Boolean(items.weapon && items.ability));
  const exactPairs = $derived(
    (hasSlot("armor") ? armorList.length : 1) * (hasSlot("ring") ? ringList.length : 1),
  );

  function options(): OptimizeOptions {
    if (mode === "exact") {
      return {
        lock: [],
        pools: ["tiered", "untiered", "set"],
        enchantments: "best",
        artifacts,
        slots: perItem,
        weights: weights(balance),
        exact: {
          armors: hasSlot("armor") ? [...armorList] : [""],
          rings: hasSlot("ring") ? [...ringList] : [""],
        },
      };
    }
    return {
      lock: usableSlots.filter((s) => lock[s]),
      pools: (["tiered", "untiered", "set"] as const).filter((p) => pools[p]),
      enchantments: enchantMode,
      artifacts,
      slots: perItem,
      around,
      weights: weights(balance),
      burst: goal === "burst" ? { ...burst } : undefined,
      results: 5,
    };
  }

  async function search() {
    if (!engine) return;
    searching = true;
    found = null;
    progress = null;
    stopped = false;
    searchedMode = mode;
    rankedBy = { balance, burst: mode === "wide" && goal === "burst", stat: aroundStat };
    searched = options();
    opened = null;
    startedAt = now = performance.now();
    const ticking = setInterval(() => (now = performance.now()), 500);
    const res = await engine.optimize({ ...$state.snapshot(request), optimize: options() }, (p) => (progress = p));
    clearInterval(ticking);
    found = res ?? asResponse(progress);
    searching = false;
  }

  async function details(i: number, b: OptimizedBuild) {
    opened = opened === i ? null : i;
    explained = null;
    if (opened === null || !engine) return;
    const res = await engine.explainBuild({
      ...$state.snapshot(request),
      weapon: b.weapon,
      ability: b.ability || undefined,
      armor: b.armor || undefined,
      ring: b.ring || undefined,
      enchantments: b.enchantments.weapon,
      gearEnchantments: { ability: b.enchantments.ability, armor: b.enchantments.armor, ring: b.enchantments.ring },
      optimize: searched ?? options(),
    });
    if (opened === i) explained = res;
  }

  // A loss without a piece, in the units of the search.
  const without = (n: number | null | undefined) =>
    n === null || n === undefined ? "" : ` (${signed(-n)}${scoreUnit || " DPS"} without it)`;
  const statWorth = (e: ExplainResponse) =>
    e.ok
      ? e.stats
          .filter((s) => Math.abs(s.perPoint) >= 0.05)
          .sort((a, b) => b.perPoint - a.perPoint)
          .map((s) => `${s.stat} ${s.perPoint > 0 ? "+" : ""}${tenths.format(s.perPoint)}`)
          .join(", ")
      : "";

  function stop() {
    stopped = true;
    engine?.stopOptimizing();
  }

  // A fresh estimate whenever the options or the build change, once they
  // settle for a moment. Only the latest one counts.
  let asked = 0;
  $effect(() => {
    const ask = { ...$state.snapshot(request), optimize: options() };
    if (!active || !engine || searching || mode === "exact" || (goal === "burst" && !burstValid)) return;
    const id = ++asked;
    const timer = setTimeout(async () => {
      const res = await engine.estimateOptimize(ask);
      if (id === asked && res) estimate = res;
    }, 400);
    return () => clearTimeout(timer);
  });

  // Seconds as a person would say them.
  const duration = (s: number) =>
    s < 1 ? "under a second" : s < 90 ? `${whole.format(s)} s` : `${whole.format(s / 60)} min`;
  const estimateText = $derived.by(() => {
    if (!estimate?.ok) return "";
    const [low, high] = estimate.seconds;
    if (high < 1) return "Takes under a second here.";
    if (duration(low) === duration(high)) return `Takes about ${duration(low)} here.`;
    if (high < 90) return `Usually takes ${whole.format(Math.max(1, low))} to ${whole.format(high)} s here.`;
    if (low >= 90) return `Usually takes ${whole.format(low / 60)} to ${whole.format(high / 60)} min here.`;
    return `Usually takes ${duration(low)} to ${duration(high)} here.`;
  });

  // Time left at the pace so far, once there's enough of it to go on.
  const secondsLeft = $derived.by(() => {
    const elapsed = (now - startedAt) / 1000;
    const done = progress?.done ?? 0;
    return done >= 0.05 && elapsed >= 1 ? (elapsed * (1 - done)) / done : null;
  });
  const shown = $derived(found ?? asResponse(progress));

  // The balance whose weights the page times casts by, when not the total.
  const castBalance = $derived(
    castWeights && !castWeights.stat
      ? balances.find((t) => weights(t).weapon === castWeights!.weapon && weights(t).ability === castWeights!.ability)
      : undefined,
  );

  function take(b: OptimizedBuild) {
    previous = {
      items: { ...items },
      enchants: structuredClone($state.snapshot(enchants)),
      castWeights: $state.snapshot(castWeights),
    };
    // The page shows the build at the casting rhythm the search scored it
    // at. A burst search casts as often as it can in each window, so the
    // sustained numbers keep the total's rhythm.
    castWeights = rankedBy.burst
      ? undefined
      : rankedBy.stat
        ? { weapon: 1, ability: 1, stat: rankedBy.stat }
        : rankedBy.balance === 0.5
          ? undefined
          : weights(rankedBy.balance);
    items = { weapon: b.weapon, ability: b.ability, armor: b.armor, ring: b.ring };
    enchants = Object.fromEntries(
      slots.map((s) => [s, [0, 1, 2, 3].map((i) => b.enchantments[s][i] ?? "")]),
    ) as Record<Slot, string[]>;
  }

  // Takes a wide search's build and proves its enchantments with the exact
  // search, on its own armor and ring.
  async function prove(b: OptimizedBuild) {
    take(b);
    mode = "exact";
    armors = [b.armor];
    rings = [b.ring];
    await tick();
    await search();
  }

  function startAdding(slot: "armor" | "ring") {
    adding = slot;
    picking = true;
  }
  function add(name: string) {
    if (adding === "armor" && !armorList.includes(name)) armors = [...armorList, name];
    if (adding === "ring" && !ringList.includes(name)) rings = [...ringList, name];
  }
  function remove(slot: "armor" | "ring", name: string) {
    if (slot === "armor") armors = armorList.filter((n) => n !== name);
    else rings = ringList.filter((n) => n !== name);
  }
  const candidateOptions = $derived([
    { key: "", name: `No ${adding}` },
    ...itemOptions(dataset, itemsFor(dataset, cls, adding), sprites),
  ]);

  function undo() {
    if (!previous) return;
    items = previous.items;
    enchants = previous.enchants;
    castWeights = previous.castWeights;
    previous = null;
  }

  const icon = (slot: Slot, name: string) =>
    itemIcon(sprites, itemsFor(dataset, cls, slot).find((i) => i.name === name)?.type);
  const enchantCount = (b: OptimizedBuild) => slots.reduce((n, s) => n + b.enchantments[s].length, 0);
  const signed = (n: number) => `${n >= 0 ? "+" : ""}${whole.format(n)}`;
  const tenths = new Intl.NumberFormat("en", { maximumFractionDigits: 1 });
  const casts = (n: number) => `${tenths.format(n)} ${n === 1 ? "cast" : "casts"}`;
</script>

<div class="optimizer">
  {#if previous}
    <div class="row">
      <button type="button" onclick={undo}>Undo taking the last build</button>
    </div>
  {/if}
  {#if castWeights}
    <p class="hint">
      The ability is cast as often as helps {castWeights.stat
        ? `the damage ${castWeights.stat} adds`
        : castBalance === undefined
          ? "the weighted DPS"
          : castText[castBalance]}, which the search ranked this build by.
      <button type="button" onclick={() => (castWeights = undefined)}>Cast for the total DPS</button>
    </p>
  {/if}
  <div class="panel">
    <fieldset class="modes">
      <legend class="visually-hidden">Search</legend>
      <label class:chosen={mode === "exact"}>
        <input type="radio" name="search-mode" value="exact" bind:group={mode} disabled={searching} />
        Exact bounded search
      </label>
      <label class:chosen={mode === "wide"}>
        <input type="radio" name="search-mode" value="wide" bind:group={mode} disabled={searching} />
        Wide search
      </label>
    </fieldset>
    {#if mode === "exact"}
      <p class="hint">
        Keeps your weapon and ability and finds the armor, ring and enchantments with the most DPS among the armors
        and rings listed here. It checks every possibility, so the build it returns is the best there is for these
        choices with this calculator's numbers. It ranks by sustained DPS. It chooses every enchantment itself, so the
        ones on your items are left out.
      </p>
      {#if !canExact}
        <p role="alert">The exact search needs a weapon and an ability: choose an ability first.</p>
      {/if}
      {#each ["armor", "ring"] as const as slot (slot)}
        {#if hasSlot(slot)}
          <fieldset class="candidates">
            <legend>{slot === "armor" ? "Armors" : "Rings"} to choose from</legend>
            {#each slot === "armor" ? armorList : ringList as name (name)}
              <span class="chip">
                {name || `No ${slot}`}
                <button type="button" aria-label={`Remove ${name || `no ${slot}`}`} onclick={() => remove(slot, name)}>
                  ×
                </button>
              </span>
            {/each}
            <button type="button" disabled={searching} onclick={() => startAdding(slot)}>Add</button>
          </fieldset>
        {/if}
      {/each}
    {:else}
      <fieldset>
        <legend>Keep</legend>
        {#each usableSlots as slot (slot)}
          <label><input type="checkbox" bind:checked={lock[slot]} /> {slotNames[slot]}</label>
        {/each}
      </fieldset>
      <fieldset>
        <legend>Try items that are</legend>
        <label><input type="checkbox" bind:checked={pools.tiered} /> Tiered</label>
        <label><input type="checkbox" bind:checked={pools.untiered} /> Untiered</label>
        <label><input type="checkbox" bind:checked={pools.set} /> Set pieces</label>
      </fieldset>
      <label class="mode">
        Enchantments
        <select bind:value={enchantMode}>
          <option value="best">Find the best</option>
          <option value="keep">Keep mine where they fit</option>
          <option value="none">None</option>
        </select>
      </label>
    {/if}
    {#if mode === "exact" || enchantMode === "best"}
      <label class="mode">
        Enchantments per item
        <select bind:value={perItem}>
          {#each [1, 2, 3, 4] as n (n)}
            <option value={n}>{n}</option>
          {/each}
        </select>
      </label>
    {/if}
    {#if mode === "exact" || enchantMode !== "none"}
      <label><input type="checkbox" bind:checked={artifacts} /> Also ones that need an artifact or engraving</label>
    {/if}
    {#if mode === "wide"}
      <label class="mode">
        Optimize for
        <select bind:value={around}>
          <option value="every stat">All damage</option>
          <option value="nothing">All damage, just improving yours (quicker)</option>
          {#each stats.filter(([id]) => classStats.includes(id)) as [id, name] (id)}
            <option value={id}>Damage that scales with {name}</option>
          {/each}
        </select>
      </label>
      {#if aroundStat}
        <p class="hint">
          Counts only the damage {statName(aroundStat)} adds to what scales with it:{aroundStat === "ATT" ||
          aroundStat === "DEX"
            ? " the weapon's shots, and"
            : ""} abilities, gear and enchantments whose damage grows with {aroundStat}. Everything else counts for
          nothing, so the weapon may be picked only for its stats; the total damage only breaks ties.
        </p>
      {/if}
      <label class="mode">
        Damage
        <select bind:value={goal}>
          <option value="sustained">Sustained DPS</option>
          <option value="burst">Burst damage</option>
        </select>
      </label>
      {#if goal === "burst"}
        <div class="burst">
          <label>A window of <input type="number" min="0" max="60" step="0.1" bind:value={burst.window} /> s</label>
          <label>every <input type="number" min="1" max="600" step="1" bind:value={burst.every} /> s</label>
        </div>
        {#if !burstValid}
          <p role="alert">The window has to be 0 to 60 seconds, and shorter than the time between windows.</p>
        {/if}
        <p class="hint">
          For bosses you can only hurt now and then. A window of 0 s is a single moment: one volley and one cast. Only
          what you fire or cast in the window counts, and of a poison only what lands in it. The ability is cast as
          the window opens and again as soon as cooldown and MP allow, and over many windows each only gets the MP
          that regenerates in between.
        </p>
      {/if}
    {/if}
    {#if !aroundStat}
      <label class="mode">
        Rank by
        <input type="range" min="0" max="1" step="0.25" bind:value={balance} />
        <span class="balance">{balanceText[balance]}</span>
      </label>
    {/if}
    {#if searching}
      <div class="progress">
        <progress max="1" value={progress?.done ?? 0}></progress>
        <span>
          {#if progress?.pairs}Armor and ring combination {Math.min(progress.pairs[0] + 1, progress.pairs[1])} of
            {progress.pairs[1]}.
          {/if}
          {secondsLeft === null
            ? "Working out how long this takes…"
            : `About ${duration(Math.max(1, secondsLeft))} left`}
        </span>
        <button type="button" onclick={stop}>Stop</button>
      </div>
    {:else}
      {#if mode === "exact"}
        <button type="button" class="primary" onclick={search} disabled={!engine || !canExact || exactPairs === 0}>
          Find the proven best
        </button>
        <p class="hint">
          {exactPairs} armor and ring {exactPairs === 1 ? "combination" : "combinations"} to search. Each usually takes a
          few seconds or less. When the damage depends on a fourth stat besides ATT, DEX and WIS, like Brutal Buckler's
          VIT, each can take a few minutes.
        </p>
      {:else}
        <button type="button" class="primary" onclick={search} disabled={!engine || (goal === "burst" && !burstValid)}>
          {goal === "burst" ? "Find the most burst damage" : "Find the most DPS"}
        </button>
        {#if estimateText}<p class="hint">{estimateText}</p>{/if}
      {/if}
    {/if}
    {#if mode === "wide"}
      <p class="hint">
        Against the target and distance below, using the calculated numbers. It first measures how each item's damage
        grows with each stat and keeps the items that are best somewhere, like a spell that wins at low WIS next to
        one that wins at high WIS. It tries the most promising sets of those with whole enchantment plans, like
        stacking ATT and turning it into DEX, then improves the best one or two slots at a time.
      </p>
      <p class="caution">
        This search approximates: to stay fast it skips most combinations, so it can miss a better build, though it
        never returns one worse than yours. "Prove it" on a result checks that build's enchantments with the exact
        search.
      </p>
    {/if}
    {#if shown && !shown.ok}
      <p role="alert">{shown.error}</p>
    {:else if shown}
      {#if searching}
        <p class="hint">Best so far:</p>
      {:else if stopped && searchedMode === "exact"}
        <p class="caution">Stopped early: this is the best build it had found, not proven.</p>
      {:else if stopped}
        <p class="hint">Stopped early. These are the best builds it had found.</p>
      {:else if found?.ok && found.proven}
        <p class="proven">
          Proven best for this weapon and ability with the armors and rings listed: no other choice of them and
          their enchantments does more.
        </p>
      {/if}
      <ol class="builds">
        {#each shown.builds as b, i (i)}
          <li>
            <div class="icons">
              {#each slots as slot (slot)}
                {#if b[slot]}<Sprite {sprites} rect={icon(slot, b[slot])} size={28} label={b[slot]} />{/if}
              {/each}
            </div>
            <div class="figures">
              {#if b.burst}
                <strong>{whole.format(b.burst.damage)} per window</strong>
                <span>
                  {whole.format(b.burst.weaponDamage)} weapon, {whole.format(b.burst.abilityDamage)} ability from
                  {casts(b.burst.casts)}, {whole.format(b.dps)} DPS sustained,
                </span>
              {:else}
                <strong>{whole.format(b.dps)} DPS</strong>
                {#if b.abilityDps > 0}
                  <span>{whole.format(b.weaponDps)} weapon, {whole.format(b.abilityDps)} ability,</span>
                {/if}
              {/if}
              <span>
                {signed(b.score - shown.baseline)}{scoreUnit} on yours{#if enchantCount(b)},
                  {enchantCount(b)} enchantments{/if}
              </span>
            </div>
            <div class="actions">
              <button type="button" onclick={() => take(b)}>Use</button>
              <!-- The exact search ranks by sustained damage, by weapon and ability. -->
              {#if searchedMode === "wide" && !rankedBy.burst && !rankedBy.stat && b.ability && !searching}
                <button type="button" onclick={() => prove(b)}>Prove it</button>
              {/if}
              <button type="button" aria-expanded={opened === i} onclick={() => details(i, b)}>
                Details
              </button>
            </div>
            {#if opened === i}
              <ul class="details">
                {#each slots as slot (slot)}
                  {@const item = b[slot] ? itemsFor(dataset, cls, slot).find((x) => x.name === b[slot]) : undefined}
                  {@const e = explained?.ok ? explained : null}
                  {#if item}
                    <li>
                      <strong>{item.name}</strong>{#if statSummary(item)}: {statSummary(item)}{/if}{#if slot !== "weapon"}{without(
                          e?.items[slot],
                        )}{/if}
                      {#each b.enchantments[slot] as id, k (k)}
                        <span>{enchantmentName(id)}{without(e?.enchantments[slot][k])}</span>
                      {/each}
                      {#each itemEffects(item) as line, k (k)}<span class="effect">{line}</span>{/each}
                    </li>
                  {/if}
                {/each}
                {#if explained?.ok}
                  {#if statWorth(explained)}
                    <li>Each point of a stat adds: {statWorth(explained)}</li>
                  {/if}
                  {#if explained.burst && explained.burst.windowCasts > 0}
                    {@const w = explained.burst}
                    <li>
                      {tenths.format(w.casts)} casts per window: the cooldown and a full MP pool fit {w.windowCasts} in
                      it{#if w.paidCasts >= 0}, and the MP regenerated between windows pays for {tenths.format(
                          w.paidCasts,
                        )}{/if}.
                    </li>
                  {/if}
                {:else if explained && !explained.ok}
                  <li role="alert">{explained.error}</li>
                {/if}
              </ul>
            {/if}
          </li>
        {/each}
      </ol>
    {/if}
  </div>
</div>

<Picker
  bind:open={picking}
  {sprites}
  title={adding === "armor" ? "Add an armor" : "Add a ring"}
  options={candidateOptions}
  filters={itemFilters}
  onpick={add}
/>

<style>
  .optimizer {
    display: grid;
    gap: 0.6rem;
  }
  .row {
    display: flex;
    gap: 0.5rem;
  }
  button {
    font: inherit;
    padding: 0.35rem 0.8rem;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: var(--page);
    color: var(--text);
    cursor: pointer;
  }
  button.primary {
    background: var(--accent);
    border-color: var(--accent);
    color: white;
  }
  button:disabled {
    opacity: 0.6;
    cursor: default;
  }
  .panel {
    display: grid;
    gap: 0.6rem;
  }
  fieldset {
    display: flex;
    flex-wrap: wrap;
    gap: 0.3rem 0.9rem;
    margin: 0;
    padding: 0;
    border: none;
  }
  legend {
    font-size: 0.85rem;
    color: var(--text-2);
    margin-bottom: 0.2rem;
  }
  label {
    display: flex;
    align-items: center;
    gap: 0.35rem;
    font-size: 0.9rem;
  }
  .mode {
    display: grid;
    gap: 0.25rem;
    color: var(--text-2);
  }
  select {
    font: inherit;
    color: var(--text);
    background: var(--page);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 0.35rem 0.5rem;
  }
  .progress {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 0.3rem 0.6rem;
    align-items: center;
  }
  .progress progress {
    width: 100%;
    accent-color: var(--accent);
  }
  .progress span {
    grid-row: 2;
    font-size: 0.85rem;
    color: var(--text-2);
  }
  .progress button {
    grid-row: 1 / span 2;
    grid-column: 2;
  }
  .burst {
    display: flex;
    flex-wrap: wrap;
    gap: 0.3rem 0.8rem;
  }
  .burst input {
    width: 4.5rem;
    font: inherit;
    color: var(--text);
    background: var(--page);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 0.25rem 0.4rem;
  }
  input[type="range"] {
    accent-color: var(--accent);
  }
  .balance {
    font-size: 0.85rem;
    color: var(--text);
  }
  .hint {
    margin: 0;
    font-size: 0.8rem;
    color: var(--text-2);
  }
  .caution,
  .proven {
    margin: 0;
    font-size: 0.85rem;
    padding: 0.4rem 0.6rem;
    border-radius: 6px;
    border-left: 3px solid var(--accent);
    background: var(--grid);
  }
  .modes {
    display: inline-flex;
    gap: 0;
    width: fit-content;
    border: 1px solid var(--border);
    border-radius: 6px;
    overflow: hidden;
  }
  .modes label {
    padding: 0.35rem 0.8rem;
    cursor: pointer;
  }
  .modes label + label {
    border-left: 1px solid var(--border);
  }
  .modes label.chosen {
    background: var(--accent);
    color: white;
  }
  .modes input {
    position: absolute;
    opacity: 0;
    pointer-events: none;
  }
  .modes label:focus-within {
    outline: 2px solid var(--accent);
    outline-offset: -2px;
  }
  .visually-hidden {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip: rect(0 0 0 0);
  }
  .candidates {
    align-items: center;
  }
  .chip {
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
    font-size: 0.85rem;
    padding: 0.1rem 0.2rem 0.1rem 0.6rem;
    border: 1px solid var(--border);
    border-radius: 999px;
  }
  .chip button {
    padding: 0 0.4rem;
    border: none;
    background: none;
  }
  .builds {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    gap: 0.4rem;
  }
  .builds > li {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr) auto;
    gap: 0.5rem;
    align-items: center;
    padding: 0.3rem 0;
    border-bottom: 1px solid var(--grid);
  }
  .icons {
    display: flex;
  }
  .actions {
    display: grid;
    gap: 0.25rem;
  }
  .details {
    grid-column: 1 / -1;
    list-style: none;
    margin: 0;
    padding: 0.2rem 0 0.3rem;
    display: grid;
    gap: 0.35rem;
    font-size: 0.78rem;
    color: var(--text-2);
  }
  .details li {
    display: block;
  }
  .details strong {
    color: var(--text);
    font-weight: 600;
  }
  .details span {
    display: block;
  }
  .details .effect {
    color: var(--muted);
  }
  .figures {
    display: grid;
    font-size: 0.85rem;
  }
  .figures span {
    color: var(--text-2);
  }
</style>
