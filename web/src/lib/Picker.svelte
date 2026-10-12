<script lang="ts" module>
  import type { Rect } from "./sprites";

  export interface PickerOption {
    key: string;
    name: string;
    icon?: Rect;
    /** A second line, like an item's stats or an enchantment's description. */
    detail?: string;
    /** Options are listed under their group's heading, in order of first use. */
    group?: string;
    /** Filter keys this option matches, like "tiered". */
    tags?: string[];
    /** Why it can't be picked, shown instead of the detail. */
    disabled?: string;
  }
</script>

<script lang="ts">
  // A searchable list of choices in a dialog, for items and enchantments.
  import Sprite from "./Sprite.svelte";
  import type { Sprites } from "./sprites";

  let {
    open = $bindable(),
    title,
    options,
    selected = "",
    filters = [],
    sprites,
    changes,
    onpick,
  }: {
    open: boolean;
    title: string;
    options: PickerOption[];
    selected?: string;
    /** Buttons that narrow the list to options with that tag. */
    filters?: { key: string; label: string }[];
    sprites: Sprites | null;
    /** Each option's DPS change, null for one that can't be worked out; still coming if null itself. */
    changes?: Map<string, number | null> | null;
    onpick: (key: string) => void;
  } = $props();

  let dialog = $state<HTMLDialogElement>();
  let search = $state("");
  let filter = $state("");
  let byDps = $state(false);
  const whole = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });
  const change = (key: string) => changes?.get(key);
  const changeText = (n: number) => (Math.abs(n) < 0.5 ? "±0" : `${n > 0 ? "+" : "−"}${whole.format(Math.abs(n))}`);

  $effect(() => {
    if (!dialog) return;
    if (open && !dialog.open) {
      search = "";
      filter = "";
      dialog.showModal();
    } else if (!open && dialog.open) {
      dialog.close();
    }
  });

  const shown = $derived.by(() => {
    const words = search.toLowerCase().split(/\s+/).filter(Boolean);
    const matching = options.filter(
      (o) =>
        (!filter || (o.tags ?? []).includes(filter)) &&
        words.every((w) => o.name.toLowerCase().includes(w) || (o.detail ?? "").toLowerCase().includes(w)),
    );
    if (!byDps || !changes) return matching;
    // Within each group, so reskins and the like stay apart and in place.
    const order = new Map<string, number>();
    for (const o of matching) {
      if (!order.has(o.group ?? "")) order.set(o.group ?? "", order.size);
    }
    const value = (o: PickerOption) => change(o.key) ?? -Infinity;
    const place = (o: PickerOption) => order.get(o.group ?? "")!;
    return [...matching].sort((a, b) => place(a) - place(b) || value(b) - value(a));
  });
  const groups = $derived.by(() => {
    const out: { name: string; options: PickerOption[] }[] = [];
    for (const o of shown) {
      const name = o.group ?? "";
      const group = out.find((g) => g.name === name) ?? out[out.push({ name, options: [] }) - 1];
      group.options.push(o);
    }
    return out;
  });

  function pick(o: PickerOption) {
    if (o.disabled) return;
    onpick(o.key);
    open = false;
  }
</script>

<dialog
  bind:this={dialog}
  aria-label={title}
  onclose={() => (open = false)}
  onclick={(e) => e.target === dialog && (open = false)}
>
  <div class="panel">
    <header>
      <h2>{title}</h2>
      <button type="button" class="close" aria-label="Close" onclick={() => (open = false)}>×</button>
    </header>
    <!-- svelte-ignore a11y_autofocus -->
    <input type="search" placeholder="Search" aria-label="Search {title}" autofocus bind:value={search} />
    {#if filters.length}
      <div class="filters" role="group" aria-label="Show">
        <button type="button" aria-pressed={filter === ""} onclick={() => (filter = "")}>All</button>
        {#each filters as f (f.key)}
          <button type="button" aria-pressed={filter === f.key} onclick={() => (filter = f.key)}>{f.label}</button>
        {/each}
      </div>
    {/if}
    {#if changes !== undefined}
      <label class="sort">
        <input type="checkbox" bind:checked={byDps} />
        Most DPS first
        {#if changes === null}<span class="detail">working out each one's DPS…</span>{/if}
      </label>
    {/if}
    <div class="list">
      {#each groups as g (g.name)}
        {#if g.name}<h3>{g.name}</h3>{/if}
        <ul>
          {#each g.options as o (o.key)}
            <li>
              <button
                type="button"
                class:selected={o.key === selected}
                aria-current={o.key === selected ? "true" : undefined}
                disabled={!!o.disabled}
                onclick={() => pick(o)}
              >
                <Sprite {sprites} rect={o.icon} size={32} />
                <span class="text">
                  <span class="name">{o.name}</span>
                  {#if o.disabled}
                    <span class="detail">{o.disabled}</span>
                  {:else if o.detail}
                    <span class="detail">{o.detail}</span>
                  {/if}
                </span>
                {#if typeof change(o.key) === "number" && o.key !== selected}
                  {@const n = change(o.key)!}
                  <span class="change" class:up={n >= 0.5} title="DPS change on your build">{changeText(n)}</span>
                {/if}
              </button>
            </li>
          {/each}
        </ul>
      {:else}
        <p class="empty">Nothing matches.</p>
      {/each}
    </div>
  </div>
</dialog>

<style>
  dialog {
    width: min(32rem, calc(100vw - 2rem));
    max-height: min(40rem, calc(100vh - 2rem));
    padding: 0;
    border: 1px solid var(--border);
    border-radius: 10px;
    background: var(--surface);
    color: var(--text);
  }
  dialog::backdrop {
    background: rgba(0, 0, 0, 0.45);
  }
  .panel {
    display: grid;
    grid-template-rows: auto auto auto auto minmax(0, 1fr);
    gap: 0.6rem;
    padding: 1rem;
    max-height: min(38rem, calc(100vh - 4rem));
  }
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  h2 {
    margin: 0;
    font-size: 1.05rem;
  }
  .close {
    font-size: 1.4rem;
    line-height: 1;
    border: none;
    background: none;
    color: var(--text-2);
    cursor: pointer;
  }
  input[type="search"] {
    font: inherit;
    padding: 0.4rem 0.6rem;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: var(--page);
    color: var(--text);
  }
  .filters {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
  }
  .filters button {
    font: inherit;
    font-size: 0.85rem;
    padding: 0.15rem 0.6rem;
    border: 1px solid var(--border);
    border-radius: 999px;
    background: var(--page);
    color: var(--text-2);
    cursor: pointer;
  }
  .filters button[aria-pressed="true"] {
    border-color: var(--accent);
    color: var(--accent);
  }
  .list {
    overflow-y: auto;
    min-height: 0;
  }
  h3 {
    margin: 0.6rem 0 0.2rem;
    font-size: 0.8rem;
    font-weight: 600;
    color: var(--text-2);
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  ul {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  li button {
    display: flex;
    gap: 0.6rem;
    align-items: center;
    width: 100%;
    padding: 0.3rem 0.4rem;
    border: 1px solid transparent;
    border-radius: 6px;
    background: none;
    color: inherit;
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  li button:hover:not(:disabled),
  li button:focus-visible {
    background: var(--accent-wash);
  }
  li button.selected {
    border-color: var(--accent);
  }
  li button:disabled {
    cursor: default;
    opacity: 0.5;
  }
  .text {
    display: grid;
    min-width: 0;
    flex: 1;
  }
  .sort {
    display: flex;
    gap: 0.4rem;
    align-items: center;
    font-size: 0.85rem;
    color: var(--text-2);
  }
  .change {
    font-size: 0.85rem;
    font-variant-numeric: tabular-nums;
    color: var(--text-2);
    white-space: nowrap;
  }
  .change.up {
    color: var(--text);
    font-weight: 600;
  }
  .name {
    font-size: 0.95rem;
  }
  .detail {
    font-size: 0.8rem;
    color: var(--text-2);
  }
  .empty {
    color: var(--text-2);
  }
</style>
