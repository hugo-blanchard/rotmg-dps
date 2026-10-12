<script lang="ts">
  import { clash, type DatasetEnchantment } from "./dataset";

  let {
    label,
    options,
    special,
    value = $bindable(),
  }: {
    /** Names the item in each slot's label, like "Ring". */
    label: string;
    options: DatasetEnchantment[];
    /** The label that marks enchantments made for this item, listed first, like WEAPON. */
    special: string;
    /** Enchantment ids, at most 4. */
    value: string[];
  } = $props();

  const slots = [0, 1, 2, 3];
  const sorted = $derived(
    [...options].sort(
      (a, b) =>
        Number(b.compatible.includes(special)) - Number(a.compatible.includes(special)) || a.name.localeCompare(b.name),
    ),
  );
  const own = $derived(sorted.filter((e) => e.compatible.includes(special)));
  const stats = $derived(sorted.filter((e) => !e.compatible.includes(special)));

  function pick(slot: number, id: string) {
    value = slots.map((i) => (i === slot ? id : (value[i] ?? ""))).filter((x) => x !== "");
  }

  // An option is off when it clashes with what the other slots hold.
  function blocked(slot: number, id: string): boolean {
    const e = sorted.find((x) => x.id === id);
    return value.some((other, i) => {
      const o = sorted.find((x) => x.id === other);
      return i !== slot && o !== undefined && e !== undefined && (o.id === e.id || clash(o, e));
    });
  }
</script>

{#each slots as slot (slot)}
  {#if slot <= value.length}
    <label>
      {label}, slot {slot + 1}
      <select value={value[slot] ?? ""} onchange={(e) => pick(slot, e.currentTarget.value)}>
        <option value="">None</option>
        {#if own.length}
          <optgroup label={label}>
            {#each own as e (e.id)}
              <option value={e.id} disabled={blocked(slot, e.id)} title={e.description}>{e.name}</option>
            {/each}
          </optgroup>
        {/if}
        <optgroup label="Stats">
          {#each stats as e (e.id)}
            <option value={e.id} disabled={blocked(slot, e.id)} title={e.description}>{e.name}</option>
          {/each}
        </optgroup>
      </select>
    </label>
  {/if}
{/each}
