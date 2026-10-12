<script lang="ts">
  // Asks the engine for the build with the most DPS against the current
  // target, changing the slots you don't keep, and lets you take one of the
  // builds it finds, with an undo.
  import Sprite from "./Sprite.svelte";
  import { itemsFor, slots, type Dataset, type DatasetClass, type Slot } from "./dataset";
  import type {
    Engine,
    EstimateResponse,
    EvaluateRequest,
    OptimizeOptions,
    OptimizeProgress,
    OptimizeResponse,
    OptimizedBuild,
    BurstWindow,
  } from "./engine";
  import { isBurstWindow } from "./engine";
  import { itemIcon, type Sprites } from "./sprites";

  let {
    engine,
    request,
    dataset,
    cls,
    sprites,
    items = $bindable(),
    enchants = $bindable(),
    burst = $bindable(),
  }: {
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
  } = $props();

  const slotNames: Record<Slot, string> = { weapon: "Weapon", ability: "Ability", armor: "Armor", ring: "Ring" };
  let open = $state(false);
  let lock = $state<Record<Slot, boolean>>({ weapon: false, ability: false, armor: false, ring: false });
  let pools = $state({ tiered: true, untiered: true, set: true });
  let enchantMode = $state<OptimizeOptions["enchantments"]>("best");
  let artifacts = $state(true);
  let perItem = $state(2);
  let around = $state("every stat");
  // From 0, the weapon only, through 0.5, both in full, to 1, the ability only.
  let balance = $state(0.5);
  let goal = $state<"sustained" | "burst">("sustained");
  // How the search whose builds are shown ranked them.
  let rankedBy = $state({ balance: 0.5, burst: false });
  let searching = $state(false);
  let found = $state<OptimizeResponse | null>(null);
  // The running search's last report, and whether the builds shown are
  // from a search that was stopped.
  let progress = $state<OptimizeProgress | null>(null);
  let stopped = $state(false);
  let startedAt = 0;
  let now = $state(0);
  let estimate = $state<EstimateResponse | null>(null);
  // The build before the last one taken, for undo.
  let previous = $state<{ items: Record<Slot, string>; enchants: Record<Slot, string[]> } | null>(null);

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

  const weights = (t: number) => ({ weapon: Math.min(1, 2 * (1 - t)), ability: Math.min(1, 2 * t) });
  const balanceText: Record<number, string> = {
    0: "Weapon only",
    0.25: "Weapon, plus half the ability",
    0.5: "Weapon and ability",
    0.75: "Ability, plus half the weapon",
    1: "Ability only",
  };
  // What a gain on the start's score is in, for the search shown.
  const scoreUnit = $derived.by(() => {
    const what = rankedBy.burst ? "damage" : "DPS";
    const units: Record<number, string> = { 0: ` weapon ${what}`, 0.5: rankedBy.burst ? " damage" : "", 1: ` ability ${what}` };
    return units[rankedBy.balance] ?? " weighted";
  });
  const burstValid = $derived(isBurstWindow(burst));

  const asResponse = (p: OptimizeProgress | null): OptimizeResponse | null =>
    p && { ok: true, baseline: p.baseline, evaluated: p.evaluated, builds: p.builds };

  function options(): OptimizeOptions {
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
    rankedBy = { balance, burst: goal === "burst" };
    startedAt = now = performance.now();
    const ticking = setInterval(() => (now = performance.now()), 500);
    const res = await engine.optimize({ ...$state.snapshot(request), optimize: options() }, (p) => (progress = p));
    clearInterval(ticking);
    found = res ?? asResponse(progress);
    searching = false;
  }

  function stop() {
    stopped = true;
    engine?.stopOptimizing();
  }

  // A fresh estimate whenever the options or the build change, once they
  // settle for a moment. Only the latest one counts.
  let asked = 0;
  $effect(() => {
    const ask = { ...$state.snapshot(request), optimize: options() };
    if (!open || !engine || searching || (goal === "burst" && !burstValid)) return;
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

  function take(b: OptimizedBuild) {
    previous = { items: { ...items }, enchants: structuredClone($state.snapshot(enchants)) };
    items = { weapon: b.weapon, ability: b.ability, armor: b.armor, ring: b.ring };
    enchants = Object.fromEntries(
      slots.map((s) => [s, [0, 1, 2, 3].map((i) => b.enchantments[s][i] ?? "")]),
    ) as Record<Slot, string[]>;
  }

  function undo() {
    if (!previous) return;
    items = previous.items;
    enchants = previous.enchants;
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
  <div class="row">
    <button type="button" class="primary" aria-expanded={open} onclick={() => (open = !open)}>
      Optimize this build
    </button>
    {#if previous}
      <button type="button" onclick={undo}>Undo</button>
    {/if}
  </div>
  {#if open}
    <div class="panel">
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
      {#if enchantMode === "best"}
        <label class="mode">
          Enchantments per item
          <select bind:value={perItem}>
            {#each [1, 2, 3, 4] as n (n)}
              <option value={n}>{n}</option>
            {/each}
          </select>
        </label>
      {/if}
      {#if enchantMode !== "none"}
        <label><input type="checkbox" bind:checked={artifacts} /> Also ones that need an artifact or engraving</label>
      {/if}
      <label class="mode">
        Build around
        <select bind:value={around}>
          <option value="every stat">Every stat</option>
          <option value="nothing">Nothing, just improve yours (quicker)</option>
          {#each stats as [id, name] (id)}
            <option value={id}>{name}</option>
          {/each}
        </select>
      </label>
      <label class="mode">
        Optimize for
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
      <label class="mode">
        Rank by
        <input type="range" min="0" max="1" step="0.25" bind:value={balance} />
        <span class="balance">{balanceText[balance]}</span>
      </label>
      {#if searching}
        <div class="progress">
          <progress max="1" value={progress?.done ?? 0}></progress>
          <span>
            {secondsLeft === null
              ? "Working out how long this takes…"
              : `About ${whole.format(Math.max(1, secondsLeft))} s left`}
          </span>
          <button type="button" onclick={stop}>Stop</button>
        </div>
      {:else}
        <button type="button" class="primary" onclick={search} disabled={!engine || (goal === "burst" && !burstValid)}>
          {goal === "burst" ? "Find the most burst damage" : "Find the most DPS"}
        </button>
        {#if estimateText}<p class="hint">{estimateText}</p>{/if}
      {/if}
      <p class="hint">
        Against the target and distance below, using the calculated numbers. It tries items one or two slots at a time
        and whole sets, and refits every item's enchantments around each change. Then it starts again from the
        ability that scales best with each stat you build around, since some builds only work when everything feeds
        that ability. It can still miss a better build, but never returns one worse than yours.
      </p>
      {#if shown && !shown.ok}
        <p role="alert">{shown.error}</p>
      {:else if shown}
        {#if searching}
          <p class="hint">Best so far:</p>
        {:else if stopped}
          <p class="hint">Stopped early. These are the best builds it had found.</p>
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
              <button type="button" onclick={() => take(b)}>Use</button>
            </li>
          {/each}
        </ol>
      {/if}
    </div>
  {/if}
</div>

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
  .builds {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    gap: 0.4rem;
  }
  .builds li {
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
  .figures {
    display: grid;
    font-size: 0.85rem;
  }
  .figures span {
    color: var(--text-2);
  }
</style>
