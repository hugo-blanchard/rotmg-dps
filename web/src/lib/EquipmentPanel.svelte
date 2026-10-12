<script lang="ts">
  // The character's equipment as the game shows it: four slots with their
  // items, each with up to four enchantments. Clicking an item, an empty
  // slot or an enchantment opens a picker for it.
  import Picker, { type PickerOption } from "./Picker.svelte";
  import Sprite from "./Sprite.svelte";
  import {
    clash,
    enchantmentSlotsOf,
    enchantmentsForItem,
    itemsFor,
    slots,
    type Dataset,
    type EnchantmentOption,
    type Item,
    type Slot,
  } from "./dataset";
  import { itemIcon, type Sprites } from "./sprites";

  let {
    dataset,
    sprites,
    classId = $bindable(),
    items = $bindable(),
    enchants = $bindable(),
  }: {
    dataset: Dataset;
    sprites: Sprites | null;
    classId: string;
    /** Item names by slot; "" for an empty slot. */
    items: Record<Slot, string>;
    /** Enchantment ids by slot, always 4, "" for an empty one. */
    enchants: Record<Slot, string[]>;
  } = $props();

  const slotNames: Record<Slot, string> = { weapon: "Weapon", ability: "Ability", armor: "Armor", ring: "Ring" };
  const cls = $derived(dataset.classes.find((c) => c.id === classId)!);
  // The slots this class has; older datasets have no armor or rings.
  const shownSlots = $derived(slots.filter((s) => itemsFor(dataset, cls, s).length > 0));
  const choices = $derived(
    Object.fromEntries(slots.map((s) => [s, itemsFor(dataset, cls, s)])) as Record<Slot, Item[]>,
  );
  const equipped = $derived(
    Object.fromEntries(slots.map((s) => [s, choices[s].find((i) => i.name === items[s])])) as Record<
      Slot,
      Item | undefined
    >,
  );
  const fitting = $derived(
    Object.fromEntries(slots.map((s) => [s, enchantmentsForItem(dataset, s, equipped[s])])) as Record<
      Slot,
      EnchantmentOption[]
    >,
  );
  const slotCount = $derived(
    Object.fromEntries(slots.map((s) => [s, enchantmentSlotsOf(dataset, equipped[s])])) as Record<Slot, number>,
  );
  const setTypes = $derived(new Set((dataset.sets ?? []).flatMap((s) => s.pieces.map((p) => p.itemType))));
  const enchantment = (id: string) => (dataset.enchantments ?? []).find((e) => e.id === id);

  const statNames: Record<string, string> = { MAXHP: "HP", MAXMP: "MP" };
  function stats(item: { statBonuses?: { stat: string; amount: number }[] }): string {
    return (item.statBonuses ?? [])
      .map((b) => `${b.amount > 0 ? "+" : ""}${b.amount} ${statNames[b.stat] ?? b.stat}`)
      .join(", ");
  }
  function slotTitle(slot: Slot, item: Item | undefined): string {
    return item ? [item.name, describe(item)].filter(Boolean).join("\n") : `Choose ${slotNames[slot].toLowerCase()}`;
  }
  function describe(item: Item): string {
    return [item.tier !== undefined ? `T${item.tier}` : "", stats(item)].filter(Boolean).join(" · ");
  }

  // What the picker is choosing, if it's open.
  type Picking = { kind: "class" } | { kind: "item"; slot: Slot } | { kind: "enchant"; slot: Slot; index: number };
  let picking = $state<Picking | null>(null);
  let open = $state(false);
  function start(p: Picking) {
    picking = p;
    open = true;
  }

  const itemFilters = [
    { key: "tiered", label: "Tiered" },
    { key: "untiered", label: "Untiered" },
    { key: "set", label: "Set pieces" },
    { key: "reskin", label: "Reskins" },
  ];

  interface PickerSetup {
    title: string;
    options: PickerOption[];
    selected: string;
    filters?: typeof itemFilters;
  }
  const picker = $derived.by((): PickerSetup => {
    if (!picking) return { title: "", options: [], selected: "" };
    if (picking.kind === "class") {
      return {
        title: "Class",
        selected: classId,
        options: [...dataset.classes]
          .sort((a, b) => a.id.localeCompare(b.id))
          .map((c) => ({ key: c.id, name: c.id, icon: sprites?.index.classes[c.id]?.stand })),
      };
    }
    const slot = picking.slot;
    if (picking.kind === "item") {
      const none: PickerOption[] = slot === "weapon" ? [] : [{ key: "", name: `No ${slotNames[slot].toLowerCase()}` }];
      // Reskins play exactly like another item, so they go last, under
      // their own heading.
      const option = (i: Item): PickerOption => ({
        key: i.name,
        name: i.name,
        icon: itemIcon(sprites, i.type),
        detail: i.reskinOf ? `Reskin of ${i.reskinOf}` : describe(i),
        group: i.reskinOf ? "Reskins" : undefined,
        tags: [
          i.tier !== undefined ? "tiered" : "untiered",
          ...(setTypes.has(i.type) ? ["set"] : []),
          ...(i.reskinOf ? ["reskin"] : []),
        ],
      });
      return {
        title: slotNames[slot],
        selected: items[slot],
        filters: itemFilters,
        options: [
          ...none,
          ...choices[slot].filter((i) => !i.reskinOf).map(option),
          ...choices[slot].filter((i) => i.reskinOf).map(option),
        ],
      };
    }
    const index = picking.index;
    const others = enchants[slot].filter((id, i) => i !== index && id).map(enchantment);
    // What rolls on the item comes first, effects then stats; what only an
    // artifact or engraving puts on it comes last.
    const groups = ["Effects", "Stats", "With an artifact or engraving"];
    return {
      title: `${items[slot]}, enchantment ${index + 1}`,
      selected: enchants[slot][index],
      options: [
        { key: "", name: "Empty" },
        ...fitting[slot]
          .map(({ enchantment: e, rolls }) => {
            const blocker = others.find((o) => o && (o.id === e.id || clash(o, e)));
            return {
              key: e.id,
              name: e.name,
              icon: sprites?.index.enchantments?.[e.id],
              detail: e.description,
              group: !rolls ? groups[2] : e.labels.includes("STAT") ? groups[1] : groups[0],
              disabled: !blocker
                ? undefined
                : blocker.id === e.id
                  ? "Already on this item"
                  : `Can't go with ${blocker.name}`,
            };
          })
          .sort((a, b) => groups.indexOf(a.group) - groups.indexOf(b.group) || a.name.localeCompare(b.name)),
      ],
    };
  });

  function pick(key: string) {
    if (!picking) return;
    if (picking.kind === "class") {
      classId = key;
    } else if (picking.kind === "item") {
      items = { ...items, [picking.slot]: key };
    } else {
      const { slot, index } = picking;
      enchants = { ...enchants, [slot]: enchants[slot].map((id, i) => (i === index ? key : id)) };
    }
  }
</script>

<section class="equipment" aria-label="Equipment">
  <button type="button" class="class" onclick={() => start({ kind: "class" })} title="Change class">
    <Sprite {sprites} rect={sprites?.index.classes[classId]?.stand} size={48} />
    <span>
      <strong>{classId}</strong>
      <span class="hint">Change class</span>
    </span>
  </button>
  <div class="slots">
    {#each shownSlots as slot (slot)}
      {@const item = equipped[slot]}
      <div class="slot">
        <button
          type="button"
          class="item"
          class:empty={!item}
          title={slotTitle(slot, item)}
          aria-label="{slotNames[slot]}: {item?.name ?? 'empty'}"
          onclick={() => start({ kind: "item", slot })}
        >
          {#if item}
            <Sprite {sprites} rect={itemIcon(sprites, item.type)} size={44} />
          {:else}
            <span class="slot-name">{slotNames[slot]}</span>
          {/if}
        </button>
        <span class="item-name">{item?.name ?? ""}</span>
        {#if item && slotCount[slot] === 0}
          <span class="hint">No enchantment slots</span>
        {:else if item}
          <div class="enchants" role="group" aria-label="{slotNames[slot]} enchantments">
            {#each enchants[slot].slice(0, slotCount[slot]) as id, index (index)}
              {@const e = id ? enchantment(id) : undefined}
              <button
                type="button"
                class="enchant"
                class:empty={!e}
                title={e ? `${e.name}\n${e.description}` : `Add an enchantment`}
                aria-label="{slotNames[slot]} enchantment {index + 1}: {e?.name ?? 'empty'}"
                onclick={() => start({ kind: "enchant", slot, index })}
              >
                {#if e}
                  {@const icon = sprites?.index.enchantments?.[e.id]}
                  {#if icon}
                    <Sprite {sprites} rect={icon} size={20} />
                  {:else}
                    <span class="initials">{e.name.split(/\s+/).map((w) => w[0]).join("").slice(0, 2)}</span>
                  {/if}
                {:else}
                  +
                {/if}
              </button>
            {/each}
          </div>
        {/if}
      </div>
    {/each}
  </div>
</section>

<Picker
  bind:open
  {sprites}
  title={picker.title}
  options={picker.options}
  selected={picker.selected}
  filters={picker.filters ?? []}
  onpick={pick}
/>

<style>
  .equipment {
    display: grid;
    gap: 0.75rem;
  }
  .class {
    display: flex;
    gap: 0.6rem;
    align-items: center;
    padding: 0.2rem 0.6rem 0.2rem 0.2rem;
    border: 1px solid transparent;
    border-radius: 8px;
    background: none;
    color: inherit;
    font: inherit;
    text-align: left;
    cursor: pointer;
    justify-self: start;
  }
  .class:hover {
    border-color: var(--border);
  }
  .class > span {
    display: grid;
  }
  .hint {
    font-size: 0.8rem;
    color: var(--text-2);
  }
  .slots {
    display: grid;
    grid-template-columns: repeat(4, minmax(0, 1fr));
    gap: 0.5rem;
  }
  .slot {
    display: grid;
    justify-items: center;
    align-content: start;
    gap: 0.3rem;
    min-width: 0;
  }
  .item {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 3.75rem;
    height: 3.75rem;
    padding: 0;
    border: 2px solid var(--axis);
    border-radius: 6px;
    background: var(--page);
    cursor: pointer;
  }
  .item:hover,
  .item:focus-visible {
    border-color: var(--accent);
  }
  .item.empty {
    border-style: dashed;
  }
  .slot-name {
    font-size: 0.7rem;
    color: var(--muted);
  }
  .item-name {
    font-size: 0.72rem;
    line-height: 1.2;
    text-align: center;
    color: var(--text-2);
    overflow-wrap: anywhere;
    min-height: 2.4em;
  }
  .enchants {
    display: grid;
    grid-template-columns: repeat(2, 1.6rem);
    gap: 0.2rem;
  }
  .enchant {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 1.6rem;
    height: 1.6rem;
    padding: 0;
    border: 1px solid var(--axis);
    border-radius: 4px;
    background: var(--page);
    color: var(--muted);
    font: inherit;
    font-size: 0.85rem;
    cursor: pointer;
  }
  .enchant.empty {
    border-style: dashed;
  }
  .enchant:hover,
  .enchant:focus-visible {
    border-color: var(--accent);
    color: var(--accent);
  }
  .initials {
    font-size: 0.6rem;
    font-weight: 600;
    color: var(--text);
  }
</style>
