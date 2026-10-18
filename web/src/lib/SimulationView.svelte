<script lang="ts">
  import { onDestroy } from "svelte";
  import type { DatasetAbility, DatasetTarget, DatasetWeapon } from "./dataset";
  import TimeChart from "./TimeChart.svelte";
  import type { Engine, EvaluateRequest, SimRun, SimState, Spread, SpreadResponse } from "./engine";
  import {
    layOut,
    placements,
    playerMovements,
    targetMovements,
    withMovement,
    type SimOptions,
  } from "./scenario";
  import type { Rect, Sprites } from "./sprites";

  let {
    engine,
    request,
    weapon,
    ability,
    className,
    sprites,
    expectedDps,
    expectedHits,
    expectedAbilityDps,
    expectedProcDps,
    expectedTotalDps,
    bosses = [],
    options = $bindable(),
    pinned = null,
    onpin,
    onunpin,
    expectedUptimes = [],
    unmodelled = false,
    advanced,
  }: {
    engine: Engine;
    request: EvaluateRequest;
    weapon: DatasetWeapon;
    ability?: DatasetAbility;
    className: string;
    sprites: Sprites | null;
    expectedDps: number;
    /** The breakdown's hits per volley, summed over subattacks. */
    expectedHits: number;
    expectedAbilityDps?: number;
    /** The weapon's on-shoot procs, like poison bursts. */
    expectedProcDps: number;
    /** Weapon, procs and ability together, with buffs at their uptime. */
    expectedTotalDps: number;
    /** Bosses whose HP the targets can take. */
    bosses?: DatasetTarget[];
    /** The scene, kept by the page so a link can carry it. */
    options: SimOptions;
    /** A build to compare with in the same scene. */
    pinned?: { request: EvaluateRequest; label: string } | null;
    onpin: () => void;
    onunpin: () => void;
    /** The share of the time the build's buffs and debuffs are up, as evaluated. */
    expectedUptimes?: { effect: string; on: "player" | "target"; share: number }[];
    /** Whether the build has effects the engine doesn't model. */
    unmodelled?: boolean;
    /** Show every option, the charts and the per-target table too. */
    advanced: boolean;
  } = $props();

  const height = 360;
  let width = $state(600);
  let canvas = $state<HTMLCanvasElement>();
  let trails: HTMLCanvasElement | null = null;

  let playing = $state(true);
  // Paused because every target died, not by the user, so a new run plays.
  let autoPaused = false;
  let speed = $state(1);
  let showHitbox = $state(true);
  let showTrails = $state(false);
  // Fit everything seen so far, or keep the player in the middle at a fixed
  // scale, which reads better when things move far.
  let follow = $state(false);
  let sim = $state<SimState | null>(null);
  let error = $state("");

  let showNumbers = $state(false);
  const targetHp = $derived(
    options.hp === "custom"
      ? Math.max(0, options.customHp)
      : (bosses.find((b) => b.id === options.hp)?.maxHitPoints ?? 0),
  );
  const distance = $derived(request.hitModel?.distance ?? 4);
  const scenario = $derived.by(() => {
    const s = withMovement(layOut(options, distance, request.hitModel?.radius ?? 0.5), options, distance);
    const invulnerable =
      options.invulnerableEvery > 0 ? { every: options.invulnerableEvery, for: options.invulnerableFor } : undefined;
    const targets = s.targets.map((t) => ({ ...t, hp: targetHp > 0 ? targetHp : undefined, invulnerable }));
    const { fullMp, respawn, randomRolls, lead } = options;
    return { ...s, targets, fullMp, respawn, randomRolls, lead };
  });

  const whole = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });
  const fine = new Intl.NumberFormat("en", { maximumFractionDigits: 2 });

  const simRequest = () => ({
    ...$state.snapshot(request),
    scenario: $state.snapshot(scenario),
    compare: pinned ? $state.snapshot(pinned.request) : undefined,
  });

  // Many seeded runs of the same fight, for how much results vary.
  const manyRuns = 100;
  let spread = $state<SpreadResponse | null>(null);
  let spreading = $state(false);
  // Bumped on every restart, so runs for a build that's since changed are dropped.
  let spreadTicket = 0;
  async function runMany() {
    const ticket = ++spreadTicket;
    spreading = true;
    // Dummies that can't die would run the whole time; half a minute is plenty.
    const seconds = targetHp > 0 ? 120 : 30;
    const result = await engine.simulateMany({ ...simRequest(), runs: manyRuns, seconds });
    if (ticket === spreadTicket) {
      spread = result;
      spreading = false;
    }
  }
  // The value a share of the way through a sorted list.
  const at = (sorted: number[], share: number) => sorted[Math.min(sorted.length - 1, Math.floor(share * sorted.length))];
  function describe(s: Spread) {
    const kill = s.timeToKill.length
      ? `kills everything in ${fine.format(at(s.timeToKill, 0.5))} s typically, ` +
        `${fine.format(at(s.timeToKill, 0.1))} to ${fine.format(at(s.timeToKill, 0.9))} s in most runs` +
        (s.unfinished ? `, and not within 2 minutes in ${s.unfinished}` : "")
      : "";
    const dps =
      `${whole.format(at(s.dps, 0.5))} DPS typically, ` +
      `${whole.format(at(s.dps, 0.1))} to ${whole.format(at(s.dps, 0.9))} in most runs`;
    return kill ? `${kill}; ${dps}` : dps;
  }

  // Restart whenever the build, the targets or the distance changes.
  $effect(() => {
    const r = simRequest();
    spread = null;
    spreading = false;
    spreadTicket++;
    engine.startSimulation(r).then((e) => {
      error = e;
      sim = null;
      flashUntil = [];
      lastHits = [];
      bounds = null;
      damageHistory = [];
      mpHistory = [];
      pinnedHistory = [];
      floaters = [];
      if (autoPaused) {
        playing = true;
        autoPaused = false;
      }
      trails?.getContext("2d")?.clearRect(0, 0, trails.width, trails.height);
    });
  });

  let frame = 0;
  let last = 0;
  let pending = false;
  // Everywhere the player and targets have been since the start, so the view
  // takes them all in without jumping around as they move.
  let bounds: [number, number, number, number] | null = null;

  // Per target: when its hit flash ends, and its hits last frame.
  let flashUntil: number[] = [];
  let lastHits: number[] = [];
  let lastVolleyMs = -1000;

  let damageHistory = $state.raw<number[]>([]);
  let mpHistory = $state.raw<number[]>([]);
  let pinnedHistory = $state.raw<number[]>([]);
  // Damage numbers rising off the targets.
  let floaters: { target: number; text: string; from: string; born: number; dx: number }[] = [];

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
    if (next.history.from === damageHistory.length) {
      damageHistory = [...damageHistory, ...next.history.damage];
      mpHistory = [...mpHistory, ...next.history.mp];
    }
    if (next.compare && next.compare.history.from === pinnedHistory.length) {
      pinnedHistory = [...pinnedHistory, ...next.compare.history.damage];
    }
    if (showNumbers) {
      const born = performance.now();
      for (const h of next.hits) {
        floaters.push({ target: h.target, text: whole.format(h.damage), from: h.from, born, dx: Math.random() - 0.5 });
      }
      floaters = floaters.filter((f) => born - f.born < 700).slice(-80);
    }
    // With nothing left alive there's nothing more to see.
    const dead = (run: { targets: { hp: number }[] }) => run.targets.every((t) => t.hp <= 0);
    if (targetHp > 0 && !options.respawn && dead(next) && (!next.compare || dead(next.compare))) {
      playing = false;
      autoPaused = true;
    }
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

  // Projectile sizes by object id, for the weapon's shots and the ability's.
  const projectileSizes = $derived(
    new Map(
      [
        ...(weapon.projectiles ?? []),
        ...(ability?.projectiles ?? []),
        ...(ability?.createdProjectiles ?? []).map((c) => c.projectile),
        ...(ability?.undead ?? []).flatMap((u) => u.projectiles),
      ].map((p) => [p.objectId, p.size ?? 100]),
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

    // Fit the player, every target, what the ability leaves on the ground and
    // a tile and a half of room around them.
    let [minX, maxX, minY, maxY] = bounds ?? [player.x, player.x, player.y, player.y];
    const ground = sim.effects.filter((e) => e.kind !== "bolt");
    for (const p of [{ x: player.x, y: player.y, radius: 0.5 }, ...targets, ...ground]) {
      minX = Math.min(minX, p.x - p.radius);
      maxX = Math.max(maxX, p.x + p.radius);
      minY = Math.min(minY, p.y - p.radius);
      maxY = Math.max(maxY, p.y + p.radius);
    }
    bounds = [minX, maxX, minY, maxY];
    const margin = 1.5;
    const fit = Math.max(8, Math.min(64, width / (maxX - minX + 2 * margin), height / (maxY - minY + 2 * margin)));
    const tile = follow ? 32 : fit;
    const [cx, cy] = follow ? [player.x, player.y] : [(minX + maxX) / 2, (minY + maxY) / 2];
    const sx = (x: number) => width / 2 + (x - cx) * tile;
    const sy = (y: number) => height / 2 - (y - cy) * tile;

    ctx.fillStyle = color("--surface");
    ctx.fillRect(0, 0, width, height);
    ctx.strokeStyle = color("--grid");
    ctx.lineWidth = 1;
    for (let y = Math.floor(cy - height / (2 * tile)); sy(y) > 0; y++) {
      ctx.beginPath();
      ctx.moveTo(0, Math.round(sy(y)) + 0.5);
      ctx.lineTo(width, Math.round(sy(y)) + 0.5);
      ctx.stroke();
    }
    // Following, the view moves with you; columns show it moving sideways.
    for (let x = Math.floor(cx - width / (2 * tile)); follow && sx(x) < width; x++) {
      ctx.beginPath();
      ctx.moveTo(Math.round(sx(x)) + 0.5, 0);
      ctx.lineTo(Math.round(sx(x)) + 0.5, height);
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

    const circle = (x: number, y: number, radius: number) => {
      ctx.beginPath();
      ctx.arc(sx(x), sy(y), radius * tile, 0, Math.PI * 2);
    };

    // What the ability left on the ground goes under everything else.
    for (const e of sim.effects) {
      if (e.kind === "poison") {
        ctx.globalAlpha = 0.2;
        ctx.fillStyle = color("--poison");
        circle(e.x, e.y, e.radius);
        ctx.fill();
        ctx.globalAlpha = 1;
        ctx.strokeStyle = color("--poison");
        ctx.lineWidth = 1.5;
        ctx.stroke();
      } else if (e.kind === "trap") {
        // The ring is how close a target has to come to set it off.
        ctx.strokeStyle = color("--trap");
        ctx.lineWidth = 1.5;
        ctx.setLineDash(e.armed ? [] : [4, 4]);
        circle(e.x, e.y, e.radius);
        ctx.stroke();
        ctx.setLineDash([]);
        ctx.fillStyle = color("--trap");
        circle(e.x, e.y, 0.15);
        ctx.fill();
      } else if (e.kind === "undead" && !(e.object && sprites?.index.undead?.[e.object])) {
        // A raised undead without a sprite to draw.
        ctx.globalAlpha = 0.35;
        ctx.fillStyle = color("--undead");
        circle(e.x, e.y, e.radius);
        ctx.fill();
        ctx.globalAlpha = 1;
        ctx.strokeStyle = color("--undead");
        ctx.lineWidth = 1.5;
        ctx.stroke();
      }
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

    // Raised undead, each with its own sprite.
    for (const e of sim.effects) {
      const rect = e.kind === "undead" && e.object ? sprites?.index.undead?.[e.object] : undefined;
      if (rect) blit(rect, sx(e.x), sy(e.y), tile * 0.9);
    }

    // Targets, sized by their hitbox; the one aimed at gets a marker. They
    // fade while invulnerable.
    targets.forEach((target, i) => {
      const flashing = performance.now() < (flashUntil[i] ?? 0);
      const size = tile * 2 * target.radius;
      ctx.globalAlpha = target.invulnerable ? 0.35 : 1;
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
      ctx.font = `${Math.max(10, Math.min(14, tile * 0.35))}px sans-serif`;
      ctx.textAlign = "center";
      if (targets.length > 1) {
        ctx.fillStyle = i === aim ? color("--accent") : color("--text-2");
        ctx.fillText(i === aim ? `${i + 1} (aim)` : String(i + 1), sx(target.x), sy(target.y) - target.radius * tile - 4);
      }
      const statuses = target.invulnerable ? ["Invulnerable", ...target.statuses] : target.statuses;
      if (statuses.length) {
        ctx.fillStyle = color("--text-2");
        ctx.fillText(statuses.join(", "), sx(target.x), sy(target.y) + target.radius * tile + 14);
      }
      ctx.globalAlpha = 1;
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
      const sprite = sprites?.index.projectiles[s.objectId];
      const size = tile * 0.8 * ((projectileSizes.get(s.objectId) ?? 100) / 100);
      if (sprite) {
        const spin = ((sprite.rotation ?? 0) * s.ageMs) / 1000;
        const degrees = s.heading - 90 + (sprite.angleCorrection ?? 0) * 45 + spin;
        blit(sprite.rect, sx(s.x), sy(s.y), size, (degrees * Math.PI) / 180);
      } else {
        ctx.fillStyle = s.from === "weapon" ? color("--accent") : color("--blast");
        ctx.beginPath();
        ctx.arc(sx(s.x), sy(s.y), Math.max(2, size / 6), 0, Math.PI * 2);
        ctx.fill();
      }
    }

    // Damage numbers rise a tile and fade over 0.7 s.
    const now = performance.now();
    ctx.textAlign = "center";
    ctx.font = `600 ${Math.max(11, Math.min(15, tile * 0.4))}px sans-serif`;
    for (const f of floaters) {
      const age = (now - f.born) / 700;
      const t = targets[f.target];
      if (age >= 1 || !t) continue;
      ctx.globalAlpha = 1 - age;
      ctx.fillStyle = color(f.from === "weapon" ? "--text" : f.from === "ability" ? "--blast" : "--poison");
      ctx.fillText(f.text, sx(t.x + f.dx * t.radius), sy(t.y + t.radius + age));
    }
    ctx.globalAlpha = 1;

    // Blasts and lightning flash over the top.
    for (const e of sim.effects) {
      if (e.kind === "blast") {
        ctx.strokeStyle = color("--blast");
        ctx.lineWidth = 3;
        circle(e.x, e.y, e.radius);
        ctx.stroke();
      } else if (e.kind === "bolt" && e.chain.length > 1) {
        ctx.strokeStyle = color("--blast");
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(sx(e.chain[0].x), sy(e.chain[0].y));
        for (const p of e.chain.slice(1)) {
          ctx.lineTo(sx(p.x), sy(p.y));
        }
        ctx.stroke();
      }
    }

    if (sim.statuses.length) {
      ctx.fillStyle = color("--text-2");
      ctx.font = `${Math.max(10, Math.min(14, tile * 0.35))}px sans-serif`;
      ctx.textAlign = "center";
      ctx.fillText(sim.statuses.join(", "), sx(player.x), sy(player.y) + tile * 0.5 + 14);
    }
  }

  $effect(() => {
    void width;
    void showHitbox;
    void showTrails;
    void follow;
    draw();
  });

  const seconds = $derived(sim?.ok ? sim.timeMs / 1000 : 0);
  // Rates count up to when the last target died, not the time spent after.
  const fighting = $derived(
    sim?.ok && !options.respawn && sim.targets.every((t) => t.killedMs !== null)
      ? Math.max(...sim.targets.map((t) => t.killedMs!)) / 1000
      : seconds,
  );
  const measuredDps = $derived(sim?.ok && fighting > 0 ? sim.totals.damage / fighting : 0);
  const aimDps = $derived(sim?.ok && fighting > 0 ? (sim.targets[sim.aim]?.damage ?? 0) / fighting : 0);
  const abilityDps = $derived(sim?.ok && fighting > 0 ? sim.ability.damage / fighting : 0);
  const procDps = $derived(sim?.ok && fighting > 0 ? sim.procs.damage / fighting : 0);
  const totalDps = $derived(sim?.ok && fighting > 0 ? sim.targets.reduce((n, t) => n + t.damage, 0) / fighting : 0);
  // Buffs on the player and debuffs on the aim target, measured so far.
  const uptimes = $derived.by(() => {
    if (!sim?.ok || seconds <= 0) return [];
    const rows = (up: Record<string, number>, on: "player" | "target") =>
      Object.entries(up).map(([effect, ms]) => ({
        effect,
        on,
        share: ms / (seconds * 1000),
        expected: expectedUptimes.find((u) => u.effect === effect && u.on === on)?.share,
      }));
    return [...rows(sim.uptimeMs, "player"), ...rows(sim.targets[sim.aim]?.uptimeMs ?? {}, "target")];
  });
  const percent = new Intl.NumberFormat("en", { style: "percent", maximumFractionDigits: 0 });

  const hasProcs = $derived(expectedProcDps > 0 || (sim?.ok && sim.procs.activations > 0));
  const bucketS = 0.1;
  // DPS over the last second, and since the start.
  const lastSecond = $derived(
    damageHistory.map((_, i) => {
      let sum = 0;
      for (let k = Math.max(0, i - 9); k <= i; k++) sum += damageHistory[k];
      return sum / (Math.min(i + 1, 10) * bucketS);
    }),
  );
  const average = (history: number[]) => {
    let sum = 0;
    return history.map((d, i) => (sum += d) / ((i + 1) * bucketS));
  };
  const sinceStart = $derived(average(damageHistory));
  const pinnedSinceStart = $derived(average(pinnedHistory));
  // The figures the comparison lines up, for this build and the pinned one.
  const runSummary = (run: Omit<SimRun, "shots" | "effects" | "hits">) => {
    const kills = run.targets.map((t) => t.killedMs).filter((k) => k !== null);
    const allDead = !options.respawn && kills.length === run.targets.length ? Math.max(...kills) / 1000 : null;
    const s = allDead ?? run.timeMs / 1000;
    const total = run.targets.reduce((n, t) => n + t.damage, 0);
    return {
      total: s > 0 ? total / s : 0,
      weapon: s > 0 ? run.totals.damage / s : 0,
      ability: s > 0 ? run.ability.damage / s : 0,
      firstKill: kills.length ? Math.min(...kills) / 1000 : null,
      allDead,
    };
  };
  const firstKillMs = $derived(
    sim?.ok ? Math.min(...sim.targets.map((t) => t.killedMs ?? Infinity)) : Infinity,
  );
  const allDeadMs = $derived(
    sim?.ok && sim.targets.every((t) => t.killedMs !== null) ? Math.max(...sim.targets.map((t) => t.killedMs!)) : null,
  );
  const measuredHits = $derived(
    sim?.ok
      ? sim.perSubattack.reduce((sum, c) => sum + (c.resolvedVolleys ? c.resolvedHits / c.resolvedVolleys : 0), 0)
      : 0,
  );
</script>

<section class="sim">
  <h2>Simulation</h2>
  <p class="hint">
    The engine fires the weapon in real time, using the same motion as the hit estimate, and casts the ability at the
    aimed dummy whenever MP allows. With one dummy the weapon's numbers settle on the ones above; with more, shots that
    don't pierce stop at the first dummy in their way, and spells, poisons and lightning reach every dummy in range.
    {#if unmodelled}What's listed above as not modelled yet is left out here too.{/if}
  </p>
  <div class="controls">
    <label>
      Targets
      <select bind:value={options.placement}>
        {#each placements as p (p.value)}
          <option value={p.value}>{p.label}</option>
        {/each}
      </select>
    </label>
    <label>
      They
      <select bind:value={options.targets}>
        {#each targetMovements as m (m.value)}
          <option value={m.value}>{m.label}</option>
        {/each}
      </select>
    </label>
    {#if options.targets !== "still"}
      <label>
        Speed
        <input type="number" min="0" max="20" step="0.5" bind:value={options.targetSpeed} />
      </label>
      <label>
        {options.targets === "circle" ? "Radius" : options.targets === "chargeRetreat" ? "Back by" : "Reach"}
        <input type="number" min="0.1" max="10" step="0.5" bind:value={options.targetRange} />
      </label>
    {/if}
    {#if options.targets === "wander"}
      <button type="button" onclick={() => (options.seed += 1)}>New route</button>
    {/if}
    {#if options.targets !== "still"}
      <label><input type="checkbox" bind:checked={options.lead} /> Lead your shots</label>
    {/if}
  </div>
  <div class="controls">
    <label>
      You
      <select bind:value={options.player}>
        {#each playerMovements as m (m.value)}
          <option value={m.value}>{m.label}</option>
        {/each}
      </select>
    </label>
    {#if options.player === "kite" || options.player === "strafe"}
      <label>
        {options.player === "kite" ? "In and out by" : "Reach"}
        <input type="number" min="0.5" max="10" step="0.5" bind:value={options.playerRange} />
      </label>
    {/if}
    {#if options.player !== "still" && sim?.ok}
      <span class="hint">at {fine.format(sim.playerSpeed)} tiles/s, from SPD</span>
    {/if}
  </div>
  <div class="controls">
    {#if options.placement !== "single"}
      <label>
        {options.placement === "boss" ? "Adds" : "How many"}
        <input type="number" min="1" max="20" bind:value={options.count} />
      </label>
      {#if options.placement !== "ring"}
        <label>
          Spacing
          <input type="number" min="0.2" max="10" step="0.1" bind:value={options.spacing} />
        </label>
      {/if}
    {/if}
  </div>
  <div class="controls">
    <button type="button" onclick={() => (playing = !playing)}>{playing ? "Pause" : "Play"}</button>
    <button type="button" onclick={step}>Step</button>
    {#if advanced}
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
    <label><input type="checkbox" bind:checked={follow} /> Follow you</label>
    {#if ability}
      <label><input type="checkbox" bind:checked={options.fullMp} /> Start with full MP</label>
    {/if}
    <label><input type="checkbox" bind:checked={showNumbers} /> Damage numbers</label>
    {/if}
  </div>
  <div class="controls">
    {#if pinned}
      <span class="hint">Comparing with <strong>{pinned.label}</strong></span>
      <button type="button" onclick={onpin}>Pin this build instead</button>
      <button type="button" onclick={onunpin}>Stop comparing</button>
    {:else}
      <button type="button" onclick={onpin}>Pin this build to compare</button>
      <span class="hint">then change the build; both run in the same scene.</span>
    {/if}
  </div>
  <div class="controls">
    <label>
      Target HP
      <select bind:value={options.hp}>
        <option value="">None, they can't die</option>
        <option value="custom">Set by hand</option>
        {#each bosses as b (b.id)}
          <option value={b.id}>{b.name} ({whole.format(b.maxHitPoints)})</option>
        {/each}
      </select>
    </label>
    {#if options.hp === "custom"}
      <input type="number" min="1" step="1000" aria-label="Target HP" bind:value={options.customHp} />
    {/if}
    {#if targetHp > 0}
      <label><input type="checkbox" bind:checked={options.respawn} /> Respawn</label>
    {/if}
    {#if advanced}
    <label>
      Invulnerable every
      <input type="number" min="0" step="1" bind:value={options.invulnerableEvery} />
      s
    </label>
    {#if options.invulnerableEvery > 0}
      <label>
        for
        <input type="number" min="0.1" step="0.5" bind:value={options.invulnerableFor} />
        s
      </label>
    {/if}
    <label><input type="checkbox" bind:checked={options.randomRolls} /> Random rolls</label>
    {#if options.randomRolls}
      <button type="button" onclick={() => (options.seed += 1)}>Reroll</button>
    {/if}
    {/if}
  </div>
  {#if ability && sim?.ok}
    <div class="mp">
      <span>MP</span>
      <div class="bar"><div style:width="{(100 * sim.mp) / Math.max(1, sim.mpMax)}%"></div></div>
      <span>{whole.format(sim.mp)} / {whole.format(sim.mpMax)}</span>
    </div>
  {/if}
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
      {#if advanced}
      <div>
        <dt>Hits per volley</dt>
        <dd>{fine.format(measuredHits)} <span>engine {fine.format(expectedHits)}</span></dd>
      </div>
      <div>
        <dt>Hits per second</dt>
        <dd>{fighting > 0 ? fine.format(sim.totals.hits / fighting) : "0"}</dd>
      </div>
      {/if}
      <div>
        <dt>{sim.targets.length > 1 ? "Weapon DPS, all targets" : "Weapon DPS"}</dt>
        <dd>{whole.format(measuredDps)} <span>engine {whole.format(expectedDps)} on one</span></dd>
      </div>
      {#if hasProcs}
        <div>
          <dt>{sim.targets.length > 1 ? "Proc DPS, all targets" : "Proc DPS"}</dt>
          <dd>{whole.format(procDps)} <span>engine {whole.format(expectedProcDps)} on one</span></dd>
        </div>
      {/if}
      {#if sim.targets.length > 1}
        <div>
          <dt>DPS on the aim target</dt>
          <dd>{whole.format(aimDps)}</dd>
        </div>
      {/if}
      {#if ability}
        <div>
          <dt>{sim.targets.length > 1 ? "Ability DPS, all targets" : "Ability DPS"}</dt>
          <dd>
            {whole.format(abilityDps)}
            {#if expectedAbilityDps !== undefined}
              <span>engine {whole.format(expectedAbilityDps)} on one, in the long run</span>
            {/if}
          </dd>
        </div>
        <div>
          <dt>Casts</dt>
          <dd>{sim.ability.casts}</dd>
        </div>
      {/if}
      {#if ability || hasProcs}
        <div>
          <dt>{sim.targets.length > 1 ? "Total DPS, all targets" : "Total DPS"}</dt>
          <dd>{whole.format(totalDps)} <span>engine {whole.format(expectedTotalDps)} on one, in the long run</span></dd>
        </div>
      {/if}
      {#if targetHp > 0}
        <div>
          <dt>{sim.targets.length > 1 ? "First kill" : "Time to kill"}</dt>
          <dd>
            {Number.isFinite(firstKillMs) ? `${fine.format(firstKillMs / 1000)} s` : "not yet"}
            {#if expectedTotalDps > 0}
              <span>engine {fine.format(targetHp / expectedTotalDps)} s at its DPS</span>
            {/if}
          </dd>
        </div>
        {#if sim.targets.length > 1 && !options.respawn}
          <div>
            <dt>All dead</dt>
            <dd>{allDeadMs !== null ? `${fine.format(allDeadMs / 1000)} s` : "not yet"}</dd>
          </div>
        {/if}
      {/if}
    </dl>
    {#if advanced && uptimes.length}
      <p class="hint">
        Up so far:
        {#each uptimes as u, i (u.on + u.effect)}
          {i ? "; " : ""}{u.effect} on {u.on === "player" ? "you" : "the aim target"}
          {percent.format(u.share)}{u.expected !== undefined ? ` (engine ${percent.format(u.expected)})` : ""}
        {/each}
      </p>
    {/if}
    {#if advanced}
    <div class="controls">
      <button type="button" onclick={runMany} disabled={spreading}>
        {spreading ? "Running…" : `Run it ${manyRuns} times`}
      </button>
      <span class="hint">with random rolls, each from its own seed</span>
    </div>
    {#if spread && !spread.ok}
      <p role="alert">{spread.error}</p>
    {:else if spread}
      <ul class="spread">
        <li><strong>{pinned ? "This build" : "Over " + spread.runs + " runs"}</strong>: {describe(spread)}.</li>
        {#if spread.compare && pinned}
          <li><strong>{pinned.label}</strong>: {describe(spread.compare)}.</li>
        {/if}
      </ul>
    {/if}
    {/if}
    {#if sim.compare && pinned}
      {@const mine = runSummary(sim)}
      {@const theirs = runSummary(sim.compare)}
      {@const when = (v: number | null) => (v === null ? "not yet" : `${fine.format(v)} s`)}
      <div class="table-scroll">
        <table>
          <thead>
            <tr>
              <th scope="col"></th>
              <th scope="col">This build</th>
              <th scope="col">{pinned.label}</th>
            </tr>
          </thead>
          <tbody>
            <tr><th scope="row">Total DPS</th><td>{whole.format(mine.total)}</td><td>{whole.format(theirs.total)}</td></tr>
            <tr><th scope="row">Weapon DPS</th><td>{whole.format(mine.weapon)}</td><td>{whole.format(theirs.weapon)}</td></tr>
            <tr>
              <th scope="row">Ability DPS</th><td>{whole.format(mine.ability)}</td><td>{whole.format(theirs.ability)}</td>
            </tr>
            {#if targetHp > 0}
              <tr><th scope="row">First kill</th><td>{when(mine.firstKill)}</td><td>{when(theirs.firstKill)}</td></tr>
              {#if sim.targets.length > 1}
                <tr><th scope="row">All dead</th><td>{when(mine.allDead)}</td><td>{when(theirs.allDead)}</td></tr>
              {/if}
            {/if}
          </tbody>
        </table>
      </div>
    {/if}
    {#if advanced && damageHistory.length > 1}
      {#if pinned && pinnedHistory.length > 1}
        <TimeChart
          caption="Average DPS since the start"
          bucketMs={100}
          formatY={(v) => whole.format(v)}
          series={[
            { label: "This build", values: sinceStart, color: "var(--accent)" },
            { label: pinned.label, values: pinnedSinceStart, color: "var(--text-2)", dashed: true },
          ]}
        />
      {:else}
        <TimeChart
          caption="DPS over time"
          bucketMs={100}
          formatY={(v) => whole.format(v)}
          series={[
            { label: "Last second", values: lastSecond, color: "var(--accent)" },
            { label: "Since the start", values: sinceStart, color: "var(--text-2)", dashed: true },
          ]}
        />
      {/if}
      {#if ability}
        <TimeChart
          caption="MP"
          bucketMs={100}
          formatY={(v) => whole.format(v)}
          series={[{ label: "MP", values: mpHistory, color: "var(--accent)" }]}
        />
      {/if}
    {/if}
    {#if advanced && sim.targets.length > 1}
      <div class="table-scroll">
        <table>
          <thead>
            <tr>
              <th scope="col">Target</th>
              <th scope="col">Distance</th>
              <th scope="col">DEF</th>
              <th scope="col">Hits</th>
              <th scope="col">DPS</th>
              {#if ability}<th scope="col">Ability DPS</th>{/if}
              {#if targetHp > 0}
                <th scope="col">HP left</th>
                <th scope="col">{options.respawn ? "Kills" : "Died at"}</th>
              {/if}
            </tr>
          </thead>
          <tbody>
            {#each sim.targets as t, i (i)}
              <tr class:aim={i === sim.aim}>
                <td>{i + 1}{i === sim.aim ? " (aim)" : ""}</td>
                <td>{fine.format(Math.hypot(t.x - sim.player.x, t.y - sim.player.y))}</td>
                <td>{t.defense}</td>
                <td>{t.hits}</td>
                <td>{whole.format(fighting > 0 ? t.damage / fighting : 0)}</td>
                {#if ability}<td>{whole.format(fighting > 0 ? t.abilityDamage / fighting : 0)}</td>{/if}
                {#if targetHp > 0}
                  <td>{whole.format(t.hp)}</td>
                  <td>{options.respawn ? t.kills : t.killedMs !== null ? `${fine.format(t.killedMs / 1000)} s` : ""}</td>
                {/if}
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
  .spread {
    margin: 0;
    padding-left: 1.2rem;
    font-size: 0.9rem;
  }
  .mp {
    display: flex;
    gap: 0.5rem;
    align-items: center;
    font-size: 0.9rem;
    font-variant-numeric: tabular-nums;
  }
  .mp .bar {
    flex: 0 1 12rem;
    height: 0.5rem;
    border-radius: 4px;
    background: var(--grid);
    overflow: hidden;
  }
  .mp .bar div {
    height: 100%;
    background: var(--accent);
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
  td:first-child,
  th[scope="row"] {
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
