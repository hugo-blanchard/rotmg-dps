<script lang="ts">
  // Asks the engine for the build with the most DPS against the current
  // target, changing the slots you don't keep, and lets you take one of the
  // builds it finds, with an undo.
  import Sprite from "./Sprite.svelte";
  import { itemsFor, slots, type Dataset, type DatasetClass, type Slot } from "./dataset";
  import type { Engine, EvaluateRequest, OptimizeOptions, OptimizeResponse, OptimizedBuild } from "./engine";
  import { itemIcon, type Sprites } from "./sprites";

  let {
    engine,
    request,
    dataset,
    cls,
    sprites,
    items = $bindable(),
    enchants = $bindable(),
  }: {
    engine: Engine | null;
    /** The current build, which the search starts from. */
    request: EvaluateRequest;
    dataset: Dataset;
    cls: DatasetClass;
    sprites: Sprites | null;
    items: Record<Slot, string>;
    enchants: Record<Slot, string[]>;
  } = $props();

  const slotNames: Record<Slot, string> = { weapon: "Weapon", ability: "Ability", armor: "Armor", ring: "Ring" };
  let open = $state(false);
  let lock = $state<Record<Slot, boolean>>({ weapon: false, ability: false, armor: false, ring: false });
  let pools = $state({ tiered: true, untiered: true, set: true });
  let enchantMode = $state<OptimizeOptions["enchantments"]>("best");
  let artifacts = $state(true);
  let perItem = $state(2);
  let searching = $state(false);
  let found = $state<OptimizeResponse | null>(null);
  // The build before the last one taken, for undo.
  let previous = $state<{ items: Record<Slot, string>; enchants: Record<Slot, string[]> } | null>(null);

  const whole = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });
  const usableSlots = $derived(slots.filter((s) => itemsFor(dataset, cls, s).length > 0));

  async function search() {
    if (!engine) return;
    searching = true;
    found = null;
    found = await engine.optimize({
      ...$state.snapshot(request),
      optimize: {
        lock: usableSlots.filter((s) => lock[s]),
        pools: (["tiered", "untiered", "set"] as const).filter((p) => pools[p]),
        enchantments: enchantMode,
        artifacts,
        slots: perItem,
        results: 5,
      },
    });
    searching = false;
  }

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
      <button type="button" class="primary" onclick={search} disabled={searching || !engine}>
        {searching ? "Searching…" : "Find the most DPS"}
      </button>
      <p class="hint">
        Against the target and distance below, using the calculated DPS. It tries items one or two slots at a time
        and whole sets, refits every item's enchantments around each change, and builds around each ability that
        scales with a stat. It can still miss a better build, but never returns one worse than yours.
      </p>
      {#if found && !found.ok}
        <p role="alert">{found.error}</p>
      {:else if found}
        <ol class="builds">
          {#each found.builds as b, i (i)}
            <li>
              <div class="icons">
                {#each slots as slot (slot)}
                  {#if b[slot]}<Sprite {sprites} rect={icon(slot, b[slot])} size={28} label={b[slot]} />{/if}
                {/each}
              </div>
              <div class="figures">
                <strong>{whole.format(b.dps)} DPS</strong>
                <span>
                  {signed(b.dps - found.baseline)} on yours{#if enchantCount(b)}, {enchantCount(b)} enchantments{/if}
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
