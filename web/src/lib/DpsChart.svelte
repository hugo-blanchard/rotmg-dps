<script lang="ts">
  import type { CurvePoint } from "./engine";

  let {
    curve,
    defense,
    onpick,
  }: { curve: CurvePoint[]; defense: number; onpick: (defense: number) => void } = $props();

  const height = 240;
  const margin = { top: 12, right: 16, bottom: 28, left: 56 };
  let width = $state(600);

  const whole = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });

  // Rounds up to 1, 2, 2.5 or 5 times a power of ten, for readable ticks.
  function niceStep(rough: number): number {
    const power = 10 ** Math.floor(Math.log10(rough));
    const scaled = rough / power;
    const nice = scaled <= 1 ? 1 : scaled <= 2 ? 2 : scaled <= 2.5 ? 2.5 : scaled <= 5 ? 5 : 10;
    return nice * power;
  }

  const maxDps = $derived(Math.max(1, ...curve.map((p) => p.dps)));
  const yStep = $derived(niceStep(maxDps / 4));
  const yMax = $derived(Math.ceil(maxDps / yStep) * yStep);
  const yTicks = $derived(Array.from({ length: Math.round(yMax / yStep) + 1 }, (_, i) => i * yStep));
  const xMin = $derived(curve[0]?.defense ?? 0);
  const xMax = $derived(curve.at(-1)?.defense ?? 150);
  const xTicks = $derived.by(() => {
    const step = niceStep((xMax - xMin) / 6);
    const ticks = [];
    for (let v = Math.ceil(xMin / step) * step; v <= xMax; v += step) ticks.push(v);
    return ticks;
  });

  const plotW = $derived(Math.max(1, width - margin.left - margin.right));
  const plotH = height - margin.top - margin.bottom;
  const x = (d: number) => margin.left + ((d - xMin) / Math.max(1, xMax - xMin)) * plotW;
  const y = (v: number) => margin.top + plotH - (v / yMax) * plotH;

  const line = $derived(curve.map((p, i) => `${i ? "L" : "M"}${x(p.defense)},${y(p.dps)}`).join(""));
  const area = $derived(
    curve.length ? `${line}L${x(xMax)},${y(0)}L${x(xMin)},${y(0)}Z` : "",
  );
  const selected = $derived(curve.find((p) => p.defense === defense));

  let hover = $state<number | null>(null);
  let focused = $state(false);
  // The pointer wins; with the keyboard, the readout follows the target DEF.
  const hovered = $derived(hover !== null ? curve[hover] : focused ? (selected ?? null) : null);

  function nearest(clientX: number, svg: SVGSVGElement): number {
    const left = svg.getBoundingClientRect().left;
    const d = xMin + ((clientX - left - margin.left) / plotW) * (xMax - xMin);
    let best = 0;
    for (let i = 1; i < curve.length; i++) {
      if (Math.abs(curve[i].defense - d) < Math.abs(curve[best].defense - d)) best = i;
    }
    return best;
  }

  function onkeydown(e: KeyboardEvent) {
    const steps: Record<string, number> = {
      ArrowRight: 1,
      ArrowUp: 1,
      ArrowLeft: -1,
      ArrowDown: -1,
      PageUp: 10,
      PageDown: -10,
    };
    if (e.key in steps) {
      e.preventDefault();
      hover = null;
      onpick(Math.min(xMax, Math.max(xMin, defense + steps[e.key])));
    } else if (e.key === "Home" || e.key === "End") {
      e.preventDefault();
      hover = null;
      onpick(e.key === "Home" ? xMin : xMax);
    }
  }
</script>

<figure class="chart" bind:clientWidth={width}>
  <figcaption>DPS against target DEF</figcaption>
  <svg
    {width}
    {height}
    role="slider"
    tabindex="0"
    aria-label="Target DEF on the DPS chart"
    aria-valuemin={xMin}
    aria-valuemax={xMax}
    aria-valuenow={defense}
    aria-valuetext="{defense} DEF, {whole.format(selected?.dps ?? 0)} DPS"
    onpointermove={(e) => (hover = nearest(e.clientX, e.currentTarget))}
    onpointerleave={() => (hover = null)}
    onfocus={() => (focused = true)}
    onblur={() => (focused = false)}
    onclick={() => hovered && onpick(hovered.defense)}
    {onkeydown}
  >
    {#each yTicks as t (t)}
      <line class="grid" x1={margin.left} x2={margin.left + plotW} y1={y(t)} y2={y(t)} />
      <text class="tick" x={margin.left - 8} y={y(t)} text-anchor="end" dominant-baseline="middle">
        {whole.format(t)}
      </text>
    {/each}
    {#each xTicks as t (t)}
      <text class="tick" x={x(t)} y={height - 8} text-anchor="middle">{t}</text>
    {/each}
    <line class="axis" x1={margin.left} x2={margin.left + plotW} y1={y(0)} y2={y(0)} />

    <path class="area" d={area} />
    <path class="line" d={line} />

    {#if hovered}
      <line class="crosshair" x1={x(hovered.defense)} x2={x(hovered.defense)} y1={margin.top} y2={y(0)} />
      <circle class="dot" cx={x(hovered.defense)} cy={y(hovered.dps)} r="4" />
    {/if}
    {#if selected}
      <circle class="dot selected" cx={x(selected.defense)} cy={y(selected.dps)} r="5" />
    {/if}
  </svg>
  {#if hovered}
    <div
      class="tooltip"
      style:left="{Math.min(x(hovered.defense) + 12, width - 140)}px"
      style:top="{Math.max(0, y(hovered.dps) - 48)}px"
    >
      <strong>{whole.format(hovered.dps)} DPS</strong>
      <span>at {hovered.defense} DEF</span>
    </div>
  {/if}
  <details>
    <summary>Show as a table</summary>
    <table>
      <thead><tr><th scope="col">DEF</th><th scope="col">DPS</th></tr></thead>
      <tbody>
        {#each curve.filter((p) => p.defense % 10 === 0) as p (p.defense)}
          <tr><td>{p.defense}</td><td>{whole.format(p.dps)}</td></tr>
        {/each}
      </tbody>
    </table>
  </details>
</figure>

<style>
  .chart {
    position: relative;
    margin: 0;
    padding: 0.75rem 0;
    min-width: 0;
  }
  figcaption {
    font-weight: 600;
    margin-bottom: 0.25rem;
  }
  svg {
    display: block;
    overflow: visible;
    cursor: crosshair;
  }
  svg:focus-visible {
    outline: 2px solid var(--accent);
    outline-offset: 4px;
    border-radius: 4px;
  }
  .grid {
    stroke: var(--grid);
    stroke-width: 1;
  }
  .axis {
    stroke: var(--axis);
    stroke-width: 1;
  }
  .tick {
    fill: var(--muted);
    font-size: 12px;
    font-variant-numeric: tabular-nums;
  }
  .area {
    fill: var(--accent-wash);
  }
  .line {
    fill: none;
    stroke: var(--accent);
    stroke-width: 2;
    stroke-linejoin: round;
    stroke-linecap: round;
  }
  .crosshair {
    stroke: var(--axis);
    stroke-width: 1;
  }
  .dot {
    fill: var(--accent);
    stroke: var(--surface);
    stroke-width: 2;
  }
  .tooltip {
    position: absolute;
    pointer-events: none;
    display: grid;
    padding: 0.35rem 0.6rem;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: var(--surface);
    font-size: 0.85rem;
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.08);
  }
  .tooltip span {
    color: var(--text-2);
  }
  details {
    margin-top: 0.5rem;
    font-size: 0.85rem;
    color: var(--text-2);
  }
  table {
    margin-top: 0.5rem;
    border-collapse: collapse;
    font-variant-numeric: tabular-nums;
  }
  th,
  td {
    padding: 0.2rem 1rem 0.2rem 0;
    text-align: right;
    border-bottom: 1px solid var(--grid);
  }
</style>
