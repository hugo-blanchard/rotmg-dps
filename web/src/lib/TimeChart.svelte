<script lang="ts">
  // Values over the simulation's time, one or more series on the same axis,
  // with a readout of every series at the pointer.

  interface Series {
    label: string;
    values: number[];
    /** A CSS colour, usually a var(). */
    color: string;
    dashed?: boolean;
  }

  let {
    caption,
    series,
    bucketMs,
    formatY,
  }: {
    caption: string;
    series: Series[];
    /** Time between values. */
    bucketMs: number;
    formatY: (y: number) => string;
  } = $props();

  const height = 160;
  const margin = { top: 10, right: 16, bottom: 26, left: 56 };
  let width = $state(600);
  // Long runs are thinned to about this many points a line.
  const maxPoints = 400;

  const tickFormat = new Intl.NumberFormat("en", { maximumFractionDigits: 1 });

  function niceStep(rough: number): number {
    const power = 10 ** Math.floor(Math.log10(rough));
    const scaled = rough / power;
    const nice = scaled <= 1 ? 1 : scaled <= 2 ? 2 : scaled <= 2.5 ? 2.5 : scaled <= 5 ? 5 : 10;
    return nice * power;
  }

  const count = $derived(Math.max(0, ...series.map((s) => s.values.length)));
  const seconds = $derived(Math.max(1, (count * bucketMs) / 1000));
  const top = $derived(Math.max(1, ...series.flatMap((s) => s.values)));
  const yStep = $derived(niceStep(top / 3));
  const yMax = $derived(Math.ceil(top / yStep) * yStep);
  const yTicks = $derived(Array.from({ length: Math.round(yMax / yStep) + 1 }, (_, i) => i * yStep));
  const xStep = $derived(niceStep(seconds / 6));
  const xTicks = $derived(Array.from({ length: Math.floor(seconds / xStep) + 1 }, (_, i) => i * xStep));

  const plotW = $derived(Math.max(1, width - margin.left - margin.right));
  const plotH = height - margin.top - margin.bottom;
  const x = (s: number) => margin.left + (s / seconds) * plotW;
  const y = (v: number) => margin.top + plotH - (v / yMax) * plotH;
  const at = (i: number) => ((i + 1) * bucketMs) / 1000;

  const paths = $derived(
    series.map((s) => {
      const every = Math.max(1, Math.ceil(s.values.length / maxPoints));
      let d = "";
      for (let i = 0; i < s.values.length; i += every) {
        d += `${d ? "L" : "M"}${x(at(i)).toFixed(1)},${y(s.values[i]).toFixed(1)}`;
      }
      return d;
    }),
  );

  let hover = $state<number | null>(null);
  function fromPointer(clientX: number, svg: SVGSVGElement): number {
    const left = svg.getBoundingClientRect().left;
    const s = ((clientX - left - margin.left) / plotW) * seconds;
    return Math.min(count - 1, Math.max(0, Math.round((s * 1000) / bucketMs) - 1));
  }
</script>

<figure class="chart" bind:clientWidth={width}>
  <figcaption>
    {caption}
    {#if series.length > 1}
      <span class="legend">
        {#each series as s (s.label)}
          <span class="key">
            <svg width="18" height="8" aria-hidden="true">
              <line
                x1="0"
                x2="18"
                y1="4"
                y2="4"
                stroke={s.color}
                stroke-width="2"
                stroke-dasharray={s.dashed ? "4 3" : undefined}
              />
            </svg>
            {s.label}
          </span>
        {/each}
      </span>
    {/if}
  </figcaption>
  <svg
    {width}
    {height}
    role="img"
    aria-label={caption}
    onpointermove={(e) => count && (hover = fromPointer(e.clientX, e.currentTarget))}
    onpointerleave={() => (hover = null)}
  >
    {#each yTicks as t (t)}
      <line class="grid" x1={margin.left} x2={margin.left + plotW} y1={y(t)} y2={y(t)} />
      <text class="tick" x={margin.left - 8} y={y(t)} text-anchor="end" dominant-baseline="middle">
        {formatY(t)}
      </text>
    {/each}
    {#each xTicks as t (t)}
      <text class="tick" x={x(t)} y={height - 6} text-anchor="middle">{tickFormat.format(t)} s</text>
    {/each}
    <line class="axis" x1={margin.left} x2={margin.left + plotW} y1={y(0)} y2={y(0)} />
    {#each series as s, i (s.label)}
      <path
        d={paths[i]}
        fill="none"
        stroke={s.color}
        stroke-width="2"
        stroke-linejoin="round"
        stroke-dasharray={s.dashed ? "6 4" : undefined}
      />
    {/each}
    {#if hover !== null}
      <line class="crosshair" x1={x(at(hover))} x2={x(at(hover))} y1={margin.top} y2={y(0)} />
    {/if}
  </svg>
  {#if hover !== null}
    <div class="tooltip" style:left="{Math.min(x(at(hover)) + 12, width - 180)}px" style:top="0px">
      <span>at {tickFormat.format(at(hover))} s</span>
      {#each series as s (s.label)}
        <strong>{s.label}: {formatY(s.values[hover] ?? 0)}</strong>
      {/each}
    </div>
  {/if}
</figure>

<style>
  .chart {
    position: relative;
    margin: 0;
    min-width: 0;
  }
  figcaption {
    display: flex;
    flex-wrap: wrap;
    gap: 0.25rem 1rem;
    align-items: center;
    font-size: 0.9rem;
    font-weight: 600;
  }
  .legend {
    display: flex;
    gap: 1rem;
    font-weight: 400;
    color: var(--text-2);
  }
  .key {
    display: inline-flex;
    gap: 0.35rem;
    align-items: center;
  }
  svg {
    display: block;
    overflow: visible;
  }
  .grid {
    stroke: var(--grid);
    stroke-width: 1;
  }
  .axis,
  .crosshair {
    stroke: var(--axis);
    stroke-width: 1;
  }
  .tick {
    fill: var(--muted);
    font-size: 12px;
    font-variant-numeric: tabular-nums;
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
    font-variant-numeric: tabular-nums;
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.08);
  }
  .tooltip span {
    color: var(--text-2);
  }
  .tooltip strong {
    font-weight: 600;
  }
</style>
