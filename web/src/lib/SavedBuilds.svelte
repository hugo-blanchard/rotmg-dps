<script lang="ts">
  // Builds kept in this browser under a name, each as its link, to open
  // again later.
  let {
    link,
    suggestedName,
    dps,
    onopen,
  }: {
    /** The current build's link query, without the leading "?". */
    link: string;
    suggestedName: string;
    /** The current build's DPS, kept with it to tell builds apart. */
    dps: number | undefined;
    onopen: (link: string) => void;
  } = $props();

  interface Saved {
    name: string;
    link: string;
    dps?: number;
  }

  const storageKey = "rotmg-dps.builds";

  // Storage can be missing or refuse, as in a private window; the list then
  // lasts until the page closes.
  function read(): Saved[] {
    try {
      const parsed: unknown = JSON.parse(localStorage.getItem(storageKey) ?? "[]");
      if (!Array.isArray(parsed)) return [];
      return parsed.filter((s) => typeof s?.name === "string" && typeof s?.link === "string");
    } catch {
      return [];
    }
  }
  function write(list: Saved[]) {
    try {
      localStorage.setItem(storageKey, JSON.stringify(list));
    } catch {
      // Kept in memory only.
    }
  }

  let saved = $state<Saved[]>(read());
  let name = $state("");
  const whole = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });

  function save() {
    const entry = { name: name.trim() || suggestedName, link, dps };
    // A name used again replaces the build saved under it.
    saved = [entry, ...saved.filter((s) => s.name !== entry.name)];
    write(saved);
    name = "";
  }

  function remove(entry: Saved) {
    saved = saved.filter((s) => s !== entry);
    write(saved);
  }
</script>

<details class="saved">
  <summary>Saved builds{saved.length ? ` (${saved.length})` : ""}</summary>
  <div class="save">
    <input type="text" placeholder={suggestedName} aria-label="Name for this build" bind:value={name} />
    <button type="button" onclick={save}>Save this build</button>
  </div>
  {#if saved.length}
    <ul>
      {#each saved as s (s.name)}
        <li>
          <button type="button" class="open" class:current={s.link === link} onclick={() => onopen(s.link)}>
            <span>{s.name}</span>
            {#if s.dps !== undefined}<span class="dps">{whole.format(s.dps)} DPS when saved</span>{/if}
          </button>
          <button type="button" class="remove" aria-label="Delete {s.name}" onclick={() => remove(s)}>×</button>
        </li>
      {/each}
    </ul>
  {:else}
    <p class="hint">Saved in this browser only. Share a build with its link instead.</p>
  {/if}
</details>

<style>
  .saved {
    font-size: 0.9rem;
  }
  summary {
    cursor: pointer;
    color: var(--text-2);
  }
  .save {
    display: flex;
    gap: 0.4rem;
    margin-top: 0.5rem;
  }
  input {
    flex: 1;
    min-width: 0;
    font: inherit;
    padding: 0.3rem 0.5rem;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: var(--page);
    color: var(--text);
  }
  button {
    font: inherit;
    padding: 0.3rem 0.7rem;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: var(--page);
    color: var(--text);
    cursor: pointer;
  }
  ul {
    list-style: none;
    margin: 0.5rem 0 0;
    padding: 0;
    display: grid;
    gap: 0.25rem;
  }
  li {
    display: flex;
    gap: 0.3rem;
  }
  .open {
    flex: 1;
    min-width: 0;
    display: grid;
    text-align: left;
  }
  .open.current {
    border-color: var(--accent);
  }
  .dps {
    font-size: 0.8rem;
    color: var(--text-2);
  }
  .remove {
    color: var(--text-2);
  }
  .hint {
    margin: 0.4rem 0 0;
    font-size: 0.8rem;
    color: var(--text-2);
  }
</style>
