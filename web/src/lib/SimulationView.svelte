<script lang="ts">
  import { onDestroy } from "svelte";
  import type { DatasetWeapon } from "./dataset";
  import type { Engine, EvaluateRequest, SimState } from "./engine";
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

  const whole = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });
  const fine = new Intl.NumberFormat("en", { maximumFractionDigits: 2 });

  // Restart whenever the build, the target or the distance changes.
  $effect(() => {
    const r = $state.snapshot(request);
    engine.startSimulation(r).then((e) => {
      error = e;
      sim = null;
      flashUntil = 0;
      lastHits = 0;
      trails?.getContext("2d")?.clearRect(0, 0, trails.width, trails.height);
    });
  });

  let frame = 0;
  let last = 0;
  let pending = false;
  let flashUntil = 0;
  let lastHits = 0;
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
    if (next.totals.hits > lastHits) {
      flashUntil = performance.now() + 120;
    }
    if (sim?.ok && next.totals.volleys > sim.totals.volleys) {
      lastVolleyMs = next.timeMs;
    }
    lastHits = next.totals.hits;
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
    const { target } = sim;

    // Fit the player, the target and a tile of room around both.
    const tile = Math.max(16, Math.min(64, (height - 40) / (target.y + 2)));
    const ox = width / 2;
    const oy = height - 20 - tile;
    const sx = (x: number) => ox + x * tile;
    const sy = (y: number) => oy - y * tile;

    ctx.fillStyle = color("--surface");
    ctx.fillRect(0, 0, width, height);
    ctx.strokeStyle = color("--grid");
    ctx.lineWidth = 1;
    for (let y = 0; sy(y) > 0; y++) {
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

    // Target
    const flashing = performance.now() < flashUntil;
    if (sprites?.index.target[2]) {
      blit(sprites.index.target, sx(target.x), sy(target.y), tile);
    } else {
      ctx.fillStyle = color("--muted");
      ctx.beginPath();
      ctx.arc(sx(target.x), sy(target.y), tile * 0.4, 0, Math.PI * 2);
      ctx.fill();
    }
    if (showHitbox || flashing) {
      ctx.strokeStyle = flashing ? color("--accent") : color("--axis");
      ctx.lineWidth = flashing ? 3 : 1;
      ctx.beginPath();
      ctx.arc(sx(target.x), sy(target.y), target.radius * tile, 0, Math.PI * 2);
      ctx.stroke();
    }

    // Player, in an attack pose for a moment after each volley.
    const frames = sprites?.index.classes[className];
    if (frames) {
      const attacking = sim.timeMs - lastVolleyMs < 120 && frames.attack.length > 0;
      const rect = attacking ? frames.attack[Math.floor(sim.timeMs / 60) % frames.attack.length] : frames.stand;
      blit(rect, sx(0), sy(0), tile);
    } else {
      ctx.fillStyle = color("--text-2");
      ctx.beginPath();
      ctx.arc(sx(0), sy(0), tile * 0.35, 0, Math.PI * 2);
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
  const measuredHits = $derived(
    sim?.ok
      ? sim.perSubattack.reduce((sum, c) => sum + (c.resolvedVolleys ? c.resolvedHits / c.resolvedVolleys : 0), 0)
      : 0,
  );
</script>

<section class="sim">
  <h2>Simulation</h2>
  <p class="hint">
    The engine fires the weapon at the target in real time, using the same motion as the hit estimate. Over time its
    numbers settle on the weapon's numbers above. The ability isn't simulated.
  </p>
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
        <dt>DPS</dt>
        <dd>{whole.format(measuredDps)} <span>engine {whole.format(expectedDps)}</span></dd>
      </div>
    </dl>
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
