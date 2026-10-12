<script lang="ts">
  import { onDestroy } from "svelte";
  import type { DatasetWeapon } from "./dataset";
  import type { Engine, EvaluateRequest, SimState } from "./engine";
  import { layOut, placements, type Placement } from "./scenario";
  import type { Rect, Sprites } from "./sprites";

  let {
    engine,
    request,
    weapon,
    className,
    sprites,
    expectedDps,
    expectedHits,
  }: {
    engine: Engine;
    request: EvaluateRequest;
    weapon: DatasetWeapon;
    className: string;
    sprites: Sprites | null;
    expectedDps: number;
    /** The breakdown's hits per volley, summed over subattacks. */
    expectedHits: number;
  } = $props();

  const height = 360;
  let width = $state(600);
  let canvas = $state<HTMLCanvasElement>();
  let trails: HTMLCanvasElement | null = null;

  let playing = $state(true);
  let speed = $state(1);
  let showHitbox = $state(true);
  let showTrails = $state(false);
  let sim = $state<SimState | null>(null);
  let error = $state("");

  let placement = $state<Placement>("single");
  let count = $state(5);
  let spacing = $state(1.5);
  const scenario = $derived(
    layOut({ placement, count, spacing }, request.hitModel?.distance ?? 4, request.hitModel?.radius ?? 0.5),
  );

  const whole = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });
  const fine = new Intl.NumberFormat("en", { maximumFractionDigits: 2 });

  // Restart whenever the build, the targets or the distance changes.
  $effect(() => {
    const r = { ...$state.snapshot(request), scenario: $state.snapshot(scenario) };
    engine.startSimulation(r).then((e) => {
      error = e;
      sim = null;
      flashUntil = [];
      lastHits = [];
      trails?.getContext("2d")?.clearRect(0, 0, trails.width, trails.height);
    });
  });

  let frame = 0;
  let last = 0;
  let pending = false;
  // Per target: when its hit flash ends, and its hits last frame.
  let flashUntil: number[] = [];
  let lastHits: number[] = [];
  let lastVolleyMs = -1000;

  async function advance(ms: number) {
    if (pending) return;
    pending = true;
    const next = await engine.advanceSimulation(ms);
    pending = false;
    if (!next.ok) {
      error = next.error;
      return;
    }
    next.targets.forEach((t, i) => {
      if (t.hits > (lastHits[i] ?? 0)) {
        flashUntil[i] = performance.now() + 120;
      }
    });
    if (sim?.ok && next.totals.volleys > sim.totals.volleys) {
      lastVolleyMs = next.timeMs;
    }
    lastHits = next.targets.map((t) => t.hits);
    sim = next;
    draw();
  }

  function loop(now: number) {
    const dt = last ? Math.min(100, now - last) : 0;
    last = now;
    if (playing && !error) {
      advance(dt * speed);
    }
    frame = requestAnimationFrame(loop);
  }
  frame = requestAnimationFrame(loop);
  onDestroy(() => cancelAnimationFrame(frame));

  function step() {
    playing = false;
    advance(1000 / 60);
  }

  // Projectile sprites by projectile id, from the weapon's projectile objects.
  const projectileSprites = $derived(
    new Map(
      (weapon.projectiles ?? []).map((p) => [p.id, { sprite: sprites?.index.projectiles[p.objectId], size: p.size }]),
    ),
  );

  function draw() {
    const ctx = canvas?.getContext("2d");
    if (!ctx || !sim?.ok) return;
    const dpr = window.devicePixelRatio || 1;
    if (canvas!.width !== Math.round(width * dpr)) {
      canvas!.width = Math.round(width * dpr);
      canvas!.height = Math.round(height * dpr);
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.imageSmoothingEnabled = false;

    const styles = getComputedStyle(canvas!);
    const color = (name: string) => styles.getPropertyValue(name).trim();
    const { player, targets, aim } = sim;

    // Fit the player, every target and a tile and a half of room around them.
    let [minX, maxX, minY, maxY] = [player.x, player.x, player.y, player.y];
    for (const t of targets) {
      minX = Math.min(minX, t.x - t.radius);
      maxX = Math.max(maxX, t.x + t.radius);
      minY = Math.min(minY, t.y - t.radius);
      maxY = Math.max(maxY, t.y + t.radius);
    }
    const margin = 1.5;
    const tile = Math.max(8, Math.min(64, width / (maxX - minX + 2 * margin), height / (maxY - minY + 2 * margin)));
    const sx = (x: number) => width / 2 + (x - (minX + maxX) / 2) * tile;
    const sy = (y: number) => height / 2 - (y - (minY + maxY) / 2) * tile;

    ctx.fillStyle = color("--surface");
    ctx.fillRect(0, 0, width, height);
    ctx.strokeStyle = color("--grid");
    ctx.lineWidth = 1;
    for (let y = Math.floor(minY - margin * 2); sy(y) > 0; y++) {
      ctx.beginPath();
      ctx.moveTo(0, Math.round(sy(y)) + 0.5);
      ctx.lineTo(width, Math.round(sy(y)) + 0.5);
      ctx.stroke();
    }

    if (showTrails) {
      if (!trails || trails.width !== canvas!.width) {
        trails = document.createElement("canvas");
        trails.width = canvas!.width;
        trails.height = canvas!.height;
      }
      const t = trails.getContext("2d")!;
      t.setTransform(1, 0, 0, 1, 0, 0);
      t.globalCompositeOperation = "destination-out";
      t.fillStyle = "rgba(0, 0, 0, 0.12)";
      t.fillRect(0, 0, trails.width, trails.height);
      t.globalCompositeOperation = "source-over";
      t.setTransform(dpr, 0, 0, dpr, 0, 0);
      t.fillStyle = color("--accent");
      for (const s of sim.shots) {
        t.fillRect(sx(s.x) - 1, sy(s.y) - 1, 2, 2);
      }
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.drawImage(trails, 0, 0);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }

    const blit = (rect: Rect, x: number, y: number, size: number, rotation = 0) => {
      const scale = size / Math.max(rect[2], rect[3]);
      ctx.save();
      ctx.translate(x, y);
      ctx.rotate(rotation);
      const [x0, y0, w, h] = rect;
      ctx.drawImage(sprites!.image, x0, y0, w, h, (-w * scale) / 2, (-h * scale) / 2, w * scale, h * scale);
      ctx.restore();
    };

    // Targets, sized by their hitbox; the one aimed at gets a marker.
    targets.forEach((target, i) => {
      const flashing = performance.now() < (flashUntil[i] ?? 0);
      const size = tile * 2 * target.radius;
      if (sprites?.index.target[2]) {
        blit(sprites.index.target, sx(target.x), sy(target.y), size);
      } else {
        ctx.fillStyle = color("--muted");
        ctx.beginPath();
        ctx.arc(sx(target.x), sy(target.y), size * 0.4, 0, Math.PI * 2);
        ctx.fill();
      }
      if (showHitbox || flashing) {
        ctx.strokeStyle = flashing ? color("--accent") : color("--axis");
        ctx.lineWidth = flashing ? 3 : 1;
        ctx.beginPath();
        ctx.arc(sx(target.x), sy(target.y), target.radius * tile, 0, Math.PI * 2);
        ctx.stroke();
      }
      if (targets.length > 1) {
        ctx.fillStyle = i === aim ? color("--accent") : color("--text-2");
        ctx.font = `${Math.max(10, Math.min(14, tile * 0.35))}px sans-serif`;
        ctx.textAlign = "center";
        ctx.fillText(i === aim ? `${i + 1} (aim)` : String(i + 1), sx(target.x), sy(target.y) - target.radius * tile - 4);
      }
    });

    // Player, in an attack pose for a moment after each volley.
    const frames = sprites?.index.classes[className];
    if (frames) {
      const attacking = sim.timeMs - lastVolleyMs < 120 && frames.attack.length > 0;
      const rect = attacking ? frames.attack[Math.floor(sim.timeMs / 60) % frames.attack.length] : frames.stand;
      blit(rect, sx(player.x), sy(player.y), tile);
    } else {
      ctx.fillStyle = color("--text-2");
      ctx.beginPath();
      ctx.arc(sx(player.x), sy(player.y), tile * 0.35, 0, Math.PI * 2);
      ctx.fill();
    }

    // Shots. Headings are degrees from straight up, turning right; sprites
    // point right unless AngleCorrection turns them by 45 degree steps.
    for (const s of sim.shots) {
      const info = projectileSprites.get(s.projectileId);
      const size = tile * 0.8 * ((info?.size ?? 100) / 100);
      if (info?.sprite) {
        const spin = ((info.sprite.rotation ?? 0) * s.ageMs) / 1000;
        const degrees = s.heading - 90 + (info.sprite.angleCorrection ?? 0) * 45 + spin;
        blit(info.sprite.rect, sx(s.x), sy(s.y), size, (degrees * Math.PI) / 180);
      } else {
        ctx.fillStyle = color("--accent");
        ctx.beginPath();
        ctx.arc(sx(s.x), sy(s.y), Math.max(2, size / 6), 0, Math.PI * 2);
        ctx.fill();
      }
    }
  }

  $effect(() => {
    void width;
    void showHitbox;
    void showTrails;
    draw();
  });

  const seconds = $derived(sim?.ok ? sim.timeMs / 1000 : 0);
  const measuredDps = $derived(sim?.ok && seconds > 0 ? sim.totals.damage / seconds : 0);
  const aimDps = $derived(sim?.ok && seconds > 0 ? (sim.targets[sim.aim]?.damage ?? 0) / seconds : 0);
  const measuredHits = $derived(
    sim?.ok
      ? sim.perSubattack.reduce((sum, c) => sum + (c.resolvedVolleys ? c.resolvedHits / c.resolvedVolleys : 0), 0)
      : 0,
  );
</script>

<section class="sim">
  <h2>Simulation</h2>
  <p class="hint">
    The engine fires the weapon in real time, using the same motion as the hit estimate. With one dummy its numbers
    settle on the weapon's numbers above; with more, shots that don't pierce stop at the first dummy in their way.
    The ability isn't simulated.
  </p>
  <div class="controls">
    <label>
      Targets
      <select bind:value={placement}>
        {#each placements as p (p.value)}
          <option value={p.value}>{p.label}</option>
        {/each}
      </select>
    </label>
    {#if placement !== "single"}
      <label>
        {placement === "boss" ? "Adds" : "How many"}
        <input type="number" min="1" max="20" bind:value={count} />
      </label>
      {#if placement !== "ring"}
        <label>
          Spacing
          <input type="number" min="0.2" max="10" step="0.1" bind:value={spacing} />
        </label>
      {/if}
    {/if}
  </div>
  <div class="controls">
    <button type="button" onclick={() => (playing = !playing)}>{playing ? "Pause" : "Play"}</button>
    <button type="button" onclick={step}>Step</button>
    <label>
      Speed
      <select bind:value={speed}>
        <option value={0.1}>0.1×</option>
        <option value={0.25}>0.25×</option>
        <option value={0.5}>0.5×</option>
        <option value={1}>1×</option>
      </select>
    </label>
    <label><input type="checkbox" bind:checked={showHitbox} /> Hitbox</label>
    <label><input type="checkbox" bind:checked={showTrails} /> Trails</label>
  </div>
  <div class="stage" bind:clientWidth={width}>
    <canvas
      bind:this={canvas}
      style:width="{width}px"
      style:height="{height}px"
      aria-label="Simulation of the weapon firing at the target"
    ></canvas>
  </div>
  {#if error}
    <p role="alert">{error}</p>
  {:else if sim?.ok}
    <dl class="counters">
      <div>
        <dt>Time</dt>
        <dd>{fine.format(seconds)} s</dd>
      </div>
      <div>
        <dt>Hits per volley</dt>
        <dd>{fine.format(measuredHits)} <span>engine {fine.format(expectedHits)}</span></dd>
      </div>
      <div>
        <dt>Hits per second</dt>
        <dd>{seconds > 0 ? fine.format(sim.totals.hits / seconds) : "0"}</dd>
      </div>
      <div>
        <dt>{sim.targets.length > 1 ? "DPS, all targets" : "DPS"}</dt>
        <dd>{whole.format(measuredDps)} <span>engine {whole.format(expectedDps)} on one</span></dd>
      </div>
      {#if sim.targets.length > 1}
        <div>
          <dt>DPS on the aim target</dt>
          <dd>{whole.format(aimDps)}</dd>
        </div>
      {/if}
    </dl>
    {#if sim.targets.length > 1}
      <div class="table-scroll">
        <table>
          <thead>
            <tr>
              <th scope="col">Target</th>
              <th scope="col">Distance</th>
              <th scope="col">DEF</th>
              <th scope="col">Hits</th>
              <th scope="col">DPS</th>
            </tr>
          </thead>
          <tbody>
            {#each sim.targets as t, i (i)}
              <tr class:aim={i === sim.aim}>
                <td>{i + 1}{i === sim.aim ? " (aim)" : ""}</td>
                <td>{fine.format(Math.hypot(t.x - sim.player.x, t.y - sim.player.y))}</td>
                <td>{t.defense}</td>
                <td>{t.hits}</td>
                <td>{whole.format(seconds > 0 ? t.damage / seconds : 0)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
  {/if}
</section>

<style>
  .sim {
    display: grid;
    gap: 0.6rem;
    min-width: 0;
  }
  h2 {
    margin: 0.5rem 0 0;
    font-size: 1rem;
  }
  .hint {
    margin: 0;
    font-size: 0.85rem;
    color: var(--text-2);
  }
  .controls {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem 1rem;
    align-items: center;
    font-size: 0.9rem;
  }
  .controls label {
    display: flex;
    gap: 0.35rem;
    align-items: center;
  }
  button,
  select {
    font: inherit;
    color: var(--text);
    background: var(--page);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 0.25rem 0.75rem;
    cursor: pointer;
  }
  .stage {
    min-width: 0;
    border: 1px solid var(--border);
    border-radius: 8px;
    overflow: hidden;
  }
  canvas {
    display: block;
  }
  input[type="number"] {
    width: 4.5rem;
    font: inherit;
    color: var(--text);
    background: var(--page);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 0.2rem 0.4rem;
  }
  .table-scroll {
    overflow-x: auto;
  }
  table {
    border-collapse: collapse;
    font-size: 0.9rem;
    font-variant-numeric: tabular-nums;
  }
  th,
  td {
    text-align: right;
    padding: 0.2rem 0.75rem;
    border-bottom: 1px solid var(--border);
  }
  th:first-child,
  td:first-child {
    text-align: left;
  }
  tr.aim td {
    color: var(--accent);
  }
  .counters {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(8rem, 1fr));
    gap: 0.5rem 1rem;
    margin: 0;
  }
  .counters dt {
    font-size: 0.8rem;
    color: var(--text-2);
  }
  .counters dd {
    margin: 0;
    font-size: 1.1rem;
    font-weight: 600;
    font-variant-numeric: tabular-nums;
  }
  .counters dd span {
    display: block;
    font-size: 0.8rem;
    font-weight: 400;
    color: var(--muted);
  }
</style>
