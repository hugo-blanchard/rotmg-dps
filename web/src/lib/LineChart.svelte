<script lang="ts">
  // One series over a numeric x axis, which doubles as a slider for that x:
  // click or use the arrow keys to pick a value.

  interface Point {
    x: number;
    y: number;
  }

  let {
    caption,
    points,
    selected,
    onpick,
    formatX,
    formatY,
    sliderLabel,
    keyStep,
    stepped = false,
    integerY = false,
    yAtLeast = 1,
    tableEvery,
  }: {
    caption: string;
    points: Point[];
    selected: number;
    onpick: (x: number) => void;
    /** Readout text for an x value, like "40 DEF". */
    formatX: (x: number) => string;
    /** Readout text for a y value, like "2,345 DPS". */
    formatY: (y: number) => string;
    sliderLabel: string;
    /** How far one arrow key press moves; Page Up and Down move ten times that. */
    keyStep: number;
    /** Draw as steps, for counts that jump between whole numbers. */
    stepped?: boolean;
    integerY?: boolean;
    yAtLeast?: number;
    /** The table view lists points whose x is a multiple of this. */
    tableEvery: number;
  } = $props();

  const height = 240;
  const margin = { top: 12, right: 16, bottom: 28, left: 56 };
  let width = $state(600);

  const tickFormat = new Intl.NumberFormat("en", { maximumFractionDigits: 2 });

  // Rounds up to 1, 2, 2.5 or 5 times a power of ten, for readable ticks.
  function niceStep(rough: number): number {
    const power = 10 ** Math.floor(Math.log10(rough));
    const scaled = rough / power;
    const nice = scaled <= 1 ? 1 : scaled <= 2 ? 2 : scaled <= 2.5 ? 2.5 : scaled <= 5 ? 5 : 10;
    return nice * power;
  }

  const top = $derived(Math.max(yAtLeast, ...points.map((p) => p.y)));
  const yStep = $derived(integerY ? Math.max(1, Math.round(niceStep(top / 4))) : niceStep(top / 4));
  const yMax = $derived(Math.ceil(top / yStep) * yStep);
  const yTicks = $derived(Array.from({ length: Math.round(yMax / yStep) + 1 }, (_, i) => i * yStep));
  const xMin = $derived(points[0]?.x ?? 0);
  const xMax = $derived(points.at(-1)?.x ?? 1);
  const xTicks = $derived.by(() => {
    const step = niceStep((xMax - xMin) / 6);
    const ticks = [];
    for (let v = Math.ceil(xMin / step) * step; v <= xMax + 1e-9; v += step) ticks.push(v);
    return ticks;
  });

  const plotW = $derived(Math.max(1, width - margin.left - margin.right));
  const plotH = height - margin.top - margin.bottom;
  const x = (v: number) => margin.left + ((v - xMin) / Math.max(1e-9, xMax - xMin)) * plotW;
  const y = (v: number) => margin.top + plotH - (v / yMax) * plotH;

  const line = $derived(
    points
      .map((p, i) => {
        if (i === 0) return `M${x(p.x)},${y(p.y)}`;
        return stepped ? `H${x(p.x)}V${y(p.y)}` : `L${x(p.x)},${y(p.y)}`;
      })
      .join(""),
  );
  const area = $derived(points.length ? `${line}L${x(xMax)},${y(0)}L${x(xMin)},${y(0)}Z` : "");

  function nearestIndex(value: number): number {
    let best = 0;
    for (let i = 1; i < points.length; i++) {
      if (Math.abs(points[i].x - value) < Math.abs(points[best].x - value)) best = i;
    }
    return best;
  }

  const selectedPoint = $derived(points.length ? points[nearestIndex(selected)] : null);

  let hover = $state<number | null>(null);
  let focused = $state(false);
  // The pointer wins; with the keyboard, the readout follows the selected value.
  const shown = $derived(hover !== null ? points[hover] : focused ? selectedPoint : null);

  function fromPointer(clientX: number, svg: SVGSVGElement): number {
    const left = svg.getBoundingClientRect().left;
    return nearestIndex(xMin + ((clientX - left - margin.left) / plotW) * (xMax - xMin));
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
    let next: number | null = null;
    if (e.key in steps) {
      next = selected + steps[e.key] * keyStep;
    } else if (e.key === "Home" || e.key === "End") {
      next = e.key === "Home" ? xMin : xMax;
    }
    if (next !== null) {
      e.preventDefault();
      hover = null;
      onpick(Math.min(xMax, Math.max(xMin, Math.round(next / keyStep) * keyStep)));
    }
  }
</script>

<figure class="chart" bind:clientWidth={width}>
  <figcaption>{caption}</figcaption>
  <svg
    {width}
    {height}
    role="slider"
    tabindex="0"
    aria-label={sliderLabel}
    aria-valuemin={xMin}
    aria-valuemax={xMax}
    aria-valuenow={selected}
    aria-valuetext="{formatX(selected)}, {formatY(selectedPoint?.y ?? 0)}"
    onpointermove={(e) => (hover = fromPointer(e.clientX, e.currentTarget))}
    onpointerleave={() => (hover = null)}
    onfocus={() => (focused = true)}
    onblur={() => (focused = false)}
    onclick={() => shown && onpick(shown.x)}
    {onkeydown}
  >
    {#each yTicks as t (t)}
      <line class="grid" x1={margin.left} x2={margin.left + plotW} y1={y(t)} y2={y(t)} />
      <text class="tick" x={margin.left - 8} y={y(t)} text-anchor="end" dominant-baseline="middle">
        {tickFormat.format(t)}
      </text>
    {/each}
    {#each xTicks as t (t)}
      <text class="tick" x={x(t)} y={height - 8} text-anchor="middle">{tickFormat.format(t)}</text>
    {/each}
    <line class="axis" x1={margin.left} x2={margin.left + plotW} y1={y(0)} y2={y(0)} />

    <path class="area" d={area} />
    <path class="line" d={line} />

    {#if shown}
      <line class="crosshair" x1={x(shown.x)} x2={x(shown.x)} y1={margin.top} y2={y(0)} />
      <circle class="dot" cx={x(shown.x)} cy={y(shown.y)} r="4" />
    {/if}
    {#if selectedPoint}
      <circle class="dot" cx={x(selectedPoint.x)} cy={y(selectedPoint.y)} r="5" />
    {/if}
  </svg>
  {#if shown}
    <div
      class="tooltip"
      style:left="{Math.min(x(shown.x) + 12, width - 160)}px"
      style:top="{Math.max(0, y(shown.y) - 48)}px"
    >
      <strong>{formatY(shown.y)}</strong>
      <span>at {formatX(shown.x)}</span>
    </div>
  {/if}
  <details>
    <summary>Show as a table</summary>
    <table>
      <tbody>
        {#each points.filter((p) => Math.abs(p.x / tableEvery - Math.round(p.x / tableEvery)) < 1e-6) as p (p.x)}
          <tr><td>{formatX(p.x)}</td><td>{formatY(p.y)}</td></tr>
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
  td {
    padding: 0.2rem 1rem 0.2rem 0;
    text-align: right;
    border-bottom: 1px solid var(--grid);
  }
</style>
