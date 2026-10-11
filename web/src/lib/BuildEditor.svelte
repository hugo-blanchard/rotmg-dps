<script lang="ts">
  import { abilitiesFor, weaponsFor, type Dataset, type DatasetClass, type DatasetWeapon } from "./dataset";
  import type { PlayerStatus, Target } from "./engine";

  let {
    dataset,
    classId = $bindable(),
    weaponName = $bindable(),
    abilityName = $bindable(),
    attack = $bindable(),
    dexterity = $bindable(),
    wisdom = $bindable(),
    status = $bindable(),
    target = $bindable(),
    distance = $bindable(),
    radius = $bindable(),
  }: {
    dataset: Dataset;
    classId: string;
    weaponName: string;
    abilityName: string;
    attack: number;
    dexterity: number;
    wisdom: number;
    status: PlayerStatus;
    target: Target;
    distance: number;
    radius: number;
  } = $props();

  const classes = $derived([...dataset.classes].sort((a, b) => a.id.localeCompare(b.id)));
  const cls = $derived(dataset.classes.find((c) => c.id === classId)!);
  const weapons = $derived(weaponsFor(dataset, cls));
  const tiered = $derived(weapons.filter((w) => w.tier !== undefined));
  const untiered = $derived(weapons.filter((w) => w.tier === undefined));
  const weapon = $derived(weapons.find((w) => w.name === weaponName));
  const abilities = $derived(abilitiesFor(dataset, cls));
  const ability = $derived(abilities.find((a) => a.name === abilityName));

  function bonus(item: Pick<DatasetWeapon, "statBonuses"> | undefined, stat: string): number {
    return item?.statBonuses?.filter((b) => b.stat === stat).reduce((sum, b) => sum + b.amount, 0) ?? 0;
  }

  const gearBonuses = $derived(
    ["ATT", "DEX", "WIS"]
      .map((stat) => [stat, bonus(weapon, stat) + bonus(ability, stat)] as const)
      .filter(([, amount]) => amount !== 0)
      .map(([stat, amount]) => `${amount} ${stat}`),
  );

  function pickClass(next: DatasetClass) {
    classId = next.id;
    weaponName = weaponsFor(dataset, next)[0]?.name ?? "";
    abilityName = abilitiesFor(dataset, next)[0]?.name ?? "";
    useClassMax(next);
  }

  function useClassMax(c: DatasetClass) {
    attack = c.attack.max;
    dexterity = c.dexterity.max;
    wisdom = c.wisdom?.max ?? 0;
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
    <label>
      Class
      <select
        value={classId}
        onchange={(e) => pickClass(dataset.classes.find((c) => c.id === e.currentTarget.value)!)}
      >
        {#each classes as c (c.id)}
          <option value={c.id}>{c.id}</option>
        {/each}
      </select>
    </label>
    <label>
      Weapon
      <select bind:value={weaponName}>
        {#if tiered.length}
          <optgroup label="Tiered">
            {#each tiered as w (w.type)}
              <option value={w.name}>{w.name} (T{w.tier})</option>
            {/each}
          </optgroup>
        {/if}
        <optgroup label="Untiered and set items">
          {#each untiered as w (w.type)}
            <option value={w.name}>{w.name}</option>
          {/each}
        </optgroup>
      </select>
    </label>
    {#if abilities.length}
      <label>
        Ability
        <select bind:value={abilityName}>
          <option value="">None</option>
          {#each abilities as a (a.type)}
            <option value={a.name}>{a.name}{a.tier !== undefined ? ` (T${a.tier})` : ""}</option>
          {/each}
        </select>
      </label>
    {/if}
  </fieldset>

  <fieldset>
    <legend>Stats without the weapon or ability</legend>
    <label class="stat">
      ATT
      <input type="range" min="0" max="150" bind:value={attack} />
      <input type="number" min="0" max="999" bind:value={attack} />
    </label>
    <label class="stat">
      DEX
      <input type="range" min="0" max="150" bind:value={dexterity} />
      <input type="number" min="0" max="999" bind:value={dexterity} />
    </label>
    {#if cls.wisdom}
      <label class="stat">
        WIS
        <input type="range" min="0" max="150" bind:value={wisdom} />
        <input type="number" min="0" max="999" bind:value={wisdom} />
      </label>
    {/if}
    <p class="hint">
      {#if gearBonuses.length}
        Your gear adds {gearBonuses.join(", ")}.
      {/if}
      <button type="button" class="link" onclick={() => useClassMax(cls)}>
        Use {cls.id} max ({cls.attack.max} ATT, {cls.dexterity.max} DEX{cls.wisdom ? `, ${cls.wisdom.max} WIS` : ""})
      </button>
    </p>
  </fieldset>

  <fieldset>
    <legend>Your status effects</legend>
    <div class="checks">
      {#each playerEffects as effect (effect.key)}
        <label><input type="checkbox" bind:checked={status[effect.key]} /> {effect.label}</label>
      {/each}
    </div>
  </fieldset>

  <fieldset>
    <legend>Target</legend>
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
    <label class="stat">
      Size
      <input type="range" min="0.25" max="2" step="0.05" bind:value={radius} />
      <input type="number" min="0.1" max="5" step="0.05" bind:value={radius} />
    </label>
    <p class="hint">Distance and size (the target's radius) are in tiles. They set how many shots the hit estimate lands.</p>
    <div class="checks">
      {#each targetEffects as effect (effect.key)}
        <label><input type="checkbox" bind:checked={target[effect.key]} /> {effect.label}</label>
      {/each}
    </div>
  </fieldset>
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
  .link {
    font: inherit;
    padding: 0;
    border: 0;
    background: none;
    color: var(--accent);
    cursor: pointer;
    text-align: left;
  }
  .link:hover {
    text-decoration: underline;
  }
</style>
