<script lang="ts">
  import EquipmentPanel from "./EquipmentPanel.svelte";
  import Optimizer from "./Optimizer.svelte";
  import { itemsFor, maxStats, slots, type Dataset, type Slot } from "./dataset";
  import type { BurstWindow, Engine, EvaluateRequest, PlayerStatus, Target, WornSet } from "./engine";
  import type { Sprites } from "./sprites";

  let {
    dataset,
    sprites,
    advanced,
    engine,
    request,
    classId = $bindable(),
    items = $bindable(),
    enchants = $bindable(),
    status = $bindable(),
    target = $bindable(),
    distance = $bindable(),
    radius = $bindable(),
    burst = $bindable(),
    wornSets,
    addedStats,
  }: {
    dataset: Dataset;
    sprites: Sprites | null;
    advanced: boolean;
    engine: Engine | null;
    /** The build as the engine sees it, for the optimizer to start from. */
    request: EvaluateRequest;
    classId: string;
    items: Record<Slot, string>;
    enchants: Record<Slot, string[]>;
    status: PlayerStatus;
    target: Target;
    distance: number;
    radius: number;
    burst: BurstWindow;
    /** The engine's view of the sets worn, for the bonus each one adds. */
    wornSets: WornSet[];
    /** Stats the engine added from enchantments and the items' relative
     * effects, after rounding. */
    addedStats: { stat: string; amount: number }[];
  } = $props();

  const cls = $derived(dataset.classes.find((c) => c.id === classId)!);
  const statNames: Record<string, string> = { MAXHP: "HP", MAXMP: "MP" };
  const signed = (n: number) => `${n > 0 ? "+" : ""}${n}`;

  // Each stat at the class max, and what gear, sets and enchantments add.
  const statRows = $derived.by(() => {
    const max = maxStats(cls);
    const base: Record<string, number> = {
      ATT: max.attack,
      DEX: max.dexterity,
      SPD: max.speed,
      WIS: max.wisdom,
      VIT: max.vitality,
      DEF: max.defense,
      MAXHP: max.maxHitPoints,
      MAXMP: max.maxMagicPoints,
    };
    const added: Record<string, number> = {};
    const add = (list: { stat: string; amount: number }[] | undefined) => {
      for (const b of list ?? []) added[b.stat] = (added[b.stat] ?? 0) + b.amount;
    };
    for (const slot of slots) {
      add(itemsFor(dataset, cls, slot).find((i) => i.name === items[slot])?.statBonuses);
    }
    for (const s of wornSets) add(s.stats);
    add(addedStats);
    const shown = advanced ? Object.keys(base) : ["ATT", "DEX", "SPD", "WIS"];
    return shown.map((stat) => ({ stat, base: base[stat], added: added[stat] ?? 0 }));
  });

  function setSummary(set: WornSet): string {
    const totals = new Map<string, number>();
    for (const b of set.stats) totals.set(b.stat, (totals.get(b.stat) ?? 0) + b.amount);
    const stats = [...totals].map(([stat, amount]) => `${signed(amount)} ${statNames[stat] ?? stat}`);
    return `${set.name}, ${set.pieces} pieces${stats.length ? `: ${stats.join(", ")}` : ""}`;
  }

  // The boss picked last, shown only while DEF still matches it.
  let bossId = $state("");
  const boss = $derived(dataset.targets?.find((t) => t.id === bossId && t.defense === target.defense));
  function pickBoss(id: string) {
    bossId = id;
    const t = dataset.targets?.find((x) => x.id === id);
    if (t) {
      target.defense = t.defense;
    }
  }

  const playerEffects: { key: keyof PlayerStatus; label: string }[] = [
    { key: "weak", label: "Weak" },
    { key: "damaging", label: "Damaging" },
    { key: "berserk", label: "Berserk" },
    { key: "dazed", label: "Dazed" },
  ];
  const targetEffects: { key: Exclude<keyof Target, "defense">; label: string }[] = [
    { key: "armored", label: "Armored" },
    { key: "armorBroken", label: "Armor Broken" },
    { key: "exposed", label: "Exposed" },
    { key: "cursed", label: "Cursed" },
  ];
</script>

<form class="editor" onsubmit={(e) => e.preventDefault()}>
  <fieldset>
    <legend>Build</legend>
    <EquipmentPanel {dataset} {sprites} {engine} {request} bind:classId bind:items bind:enchants />
    <Optimizer {engine} {request} {dataset} {cls} {sprites} bind:items bind:enchants bind:burst />
  </fieldset>

  <fieldset>
    <legend>Stats</legend>
    <dl class="stats">
      {#each statRows as row (row.stat)}
        <div>
          <dt>{statNames[row.stat] ?? row.stat}</dt>
          <dd>
            {row.base + row.added}
            {#if row.added}<span class="added">{signed(row.added)}</span>{/if}
          </dd>
        </div>
      {/each}
    </dl>
    {#each wornSets.filter((s) => s.pieces >= 2) as s (s.type)}
      <p class="hint">{setSummary(s)}</p>
    {/each}
    <p class="hint">At the {cls.id} max, with what your gear adds.</p>
  </fieldset>

  <fieldset>
    <legend>Target</legend>
    {#if dataset.targets?.length}
      <label>
        Boss
        <select value={boss?.id ?? ""} onchange={(e) => pickBoss(e.currentTarget.value)}>
          <option value="">Custom DEF</option>
          {#each dataset.targets as t (t.id)}
            <option value={t.id}>{t.name} ({t.defense} DEF)</option>
          {/each}
        </select>
      </label>
    {/if}
    <label class="stat">
      DEF
      <input type="range" min="0" max="150" bind:value={target.defense} />
      <input type="number" min="0" max="999" bind:value={target.defense} />
    </label>
    <label class="stat">
      Distance
      <input type="range" min="0.5" max="10" step="0.25" bind:value={distance} />
      <input type="number" min="0.5" max="20" step="0.25" bind:value={distance} />
    </label>
    {#if advanced}
      <label class="stat">
        Size
        <input type="range" min="0.25" max="2" step="0.05" bind:value={radius} />
        <input type="number" min="0.1" max="5" step="0.05" bind:value={radius} />
      </label>
      <p class="hint">
        Distance and size (the target's radius) are in tiles. They set how many shots the hit estimate lands.
      </p>
      <div class="checks">
        {#each targetEffects as effect (effect.key)}
          <label><input type="checkbox" bind:checked={target[effect.key]} /> {effect.label}</label>
        {/each}
      </div>
    {/if}
  </fieldset>

  {#if advanced}
    <fieldset>
      <legend>Your status effects</legend>
      <div class="checks">
        {#each playerEffects as effect (effect.key)}
          <label><input type="checkbox" bind:checked={status[effect.key]} /> {effect.label}</label>
        {/each}
      </div>
    </fieldset>
  {/if}
</form>

<style>
  .editor {
    display: grid;
    gap: 1rem;
  }
  fieldset {
    margin: 0;
    padding: 0.75rem 1rem 1rem;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: var(--surface);
    display: grid;
    gap: 0.6rem;
    min-width: 0;
  }
  legend {
    padding: 0 0.25rem;
    font-weight: 600;
  }
  label {
    display: grid;
    gap: 0.25rem;
    font-size: 0.9rem;
    color: var(--text-2);
  }
  select,
  input[type="number"] {
    font: inherit;
    color: var(--text);
    background: var(--page);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 0.35rem 0.5rem;
    min-width: 0;
  }
  .stat {
    grid-template-columns: 4.5rem 1fr 4.5rem;
    align-items: center;
    gap: 0.6rem;
  }
  input[type="range"] {
    accent-color: var(--accent);
    min-width: 0;
  }
  .stats {
    display: grid;
    grid-template-columns: repeat(4, minmax(0, 1fr));
    gap: 0.5rem;
    margin: 0;
  }
  .stats dt {
    font-size: 0.75rem;
    color: var(--text-2);
  }
  .stats dd {
    margin: 0;
    font-weight: 600;
    font-variant-numeric: tabular-nums;
  }
  .added {
    font-size: 0.75rem;
    font-weight: 400;
    color: var(--accent);
  }
  .checks {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.35rem 1rem;
  }
  .checks label {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    color: var(--text);
  }
  .hint {
    margin: 0;
    font-size: 0.85rem;
    color: var(--text-2);
  }
</style>
