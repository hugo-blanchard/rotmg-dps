<script lang="ts">
  // A slider with two handles for the closest and farthest distances, in
  // tiles. Two range inputs share one track; only their handles take the
  // pointer, so either can be dragged and each keeps its own keyboard keys.
  // When the handles meet, the one that can still move away is on top.
  let {
    from = $bindable(),
    to = $bindable(),
    min = 0.5,
    max = 12,
    step = 0.25,
    disabled = false,
  }: { from: number; to: number; min?: number; max?: number; step?: number; disabled?: boolean } = $props();

  const at = (v: number) => ((v - min) / (max - min)) * 100;
  const tiles = new Intl.NumberFormat("en", { maximumFractionDigits: 2 });

  function setFrom(e: Event) {
    from = Math.min(Number((e.target as HTMLInputElement).value), to);
    (e.target as HTMLInputElement).value = String(from);
  }
  function setTo(e: Event) {
    to = Math.max(Number((e.target as HTMLInputElement).value), from);
    (e.target as HTMLInputElement).value = String(to);
  }
</script>

<div class="range">
  <div class="track" style:--from={`${at(from)}%`} style:--to={`${at(to)}%`}>
    <input
      type="range"
      {min}
      {max}
      {step}
      value={from}
      {disabled}
      aria-label="Closest distance in tiles"
      style:z-index={from > (min + max) / 2 ? 2 : 1}
      oninput={setFrom}
    />
    <input type="range" {min} {max} {step} value={to} {disabled} aria-label="Farthest distance in tiles" oninput={setTo} />
  </div>
  <span class="value">
    {from === to ? `${tiles.format(from)} tiles` : `${tiles.format(from)} to ${tiles.format(to)} tiles`}
  </span>
</div>

<style>
  .range {
    display: grid;
    gap: 0.2rem;
  }
  .track {
    position: relative;
    height: 1.4rem;
  }
  .track::before,
  .track::after {
    content: "";
    position: absolute;
    top: 50%;
    height: 4px;
    margin-top: -2px;
    border-radius: 2px;
  }
  .track::before {
    left: 0;
    right: 0;
    background: var(--border);
  }
  .track::after {
    left: var(--from);
    right: calc(100% - var(--to));
    background: var(--accent);
  }
  input {
    position: absolute;
    inset: 0;
    width: 100%;
    margin: 0;
    background: none;
    pointer-events: none;
    appearance: none;
    -webkit-appearance: none;
    z-index: 1;
  }
  input::-webkit-slider-runnable-track {
    background: none;
  }
  input::-moz-range-track {
    background: none;
  }
  input::-webkit-slider-thumb {
    -webkit-appearance: none;
    pointer-events: auto;
    width: 1rem;
    height: 1rem;
    border-radius: 50%;
    background: var(--accent);
    border: 2px solid var(--page);
    cursor: pointer;
  }
  input::-moz-range-thumb {
    pointer-events: auto;
    width: 1rem;
    height: 1rem;
    border-radius: 50%;
    background: var(--accent);
    border: 2px solid var(--page);
    cursor: pointer;
  }
  input:focus-visible::-webkit-slider-thumb {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  input:focus-visible::-moz-range-thumb {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  .value {
    font-size: 0.85rem;
    color: var(--text);
  }
</style>
