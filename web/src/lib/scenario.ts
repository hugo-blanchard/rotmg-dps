// Where the simulation's targets stand. The engine takes plain positions in
// tiles, with the player at the origin facing +y; these presets lay them
// out around the distance the hit estimate uses.

/** How something moves; see Movement in engine/include/rotmg/sim.hpp. */
export interface Movement {
  kind: MovementKind;
  /** Tiles per second, targets only: the player moves at its SPD speed. */
  speed?: number;
  range?: number;
  far?: number;
  clockwise?: boolean;
}

export type MovementKind =
  | "still"
  | "strafe"
  | "circle"
  | "chargeRetreat"
  | "wander"
  | "keepDistance"
  | "orbit"
  | "kite";

export interface ScenarioTarget {
  x: number;
  y: number;
  radius?: number;
  movement?: Movement;
  /** Hit points; none for a dummy that can't die. */
  hp?: number;
  /** Seconds it's invulnerable for, every so many seconds. */
  invulnerable?: { every: number; for: number };
}

export interface Scenario {
  player: { x: number; y: number; movement?: Movement };
  /** Index of the target the player aims at. */
  aim: number;
  targets: ScenarioTarget[];
  seed?: number;
  /** Whether the MP bar starts full; empty shows the steady state. */
  fullMp?: boolean;
  /** Whether targets that die come straight back. */
  respawn?: boolean;
  /** Roll damage and proc chances instead of using averages. */
  randomRolls?: boolean;
  /** Aim where a moving target will be when the shot gets there. */
  lead?: boolean;
}

export const targetMovements: { value: MovementKind; label: string }[] = [
  { value: "still", label: "Stand still" },
  { value: "strafe", label: "Strafe side to side" },
  { value: "circle", label: "Circle" },
  { value: "chargeRetreat", label: "Charge and retreat" },
  { value: "wander", label: "Wander" },
];

export const playerMovements: { value: MovementKind; label: string }[] = [
  { value: "still", label: "Stand still" },
  { value: "keepDistance", label: "Keep your distance" },
  { value: "orbit", label: "Circle the target" },
  { value: "kite", label: "Step in and out" },
  { value: "strafe", label: "Strafe side to side" },
];

export interface MovementOptions {
  targets: MovementKind;
  /** Tiles per second for the targets. */
  targetSpeed: number;
  /** How far targets go, in tiles: strafe or wander reach, circle radius. */
  targetRange: number;
  player: MovementKind;
  /** How far the player strafes, or how far in and out it steps. */
  playerRange: number;
  seed: number;
}

/**
 * Puts the chosen movements on a laid-out scenario. Charging targets go
 * between 1.5 tiles from the player and their start plus `targetRange`; the
 * player keeps or circles at the hit model's distance, or steps
 * `playerRange` tiles in and out of it.
 */
export function withMovement(scenario: Scenario, options: MovementOptions, distance: number): Scenario {
  const targets = scenario.targets.map((t) => {
    if (options.targets === "still") return t;
    const start = Math.hypot(t.x - scenario.player.x, t.y - scenario.player.y);
    const movement: Movement =
      options.targets === "chargeRetreat"
        ? { kind: "chargeRetreat", speed: options.targetSpeed, range: 1.5, far: start + options.targetRange }
        : { kind: options.targets, speed: options.targetSpeed, range: options.targetRange };
    return { ...t, movement };
  });
  const near = Math.max(1, distance - options.playerRange);
  const player: Movement =
    options.player === "kite"
      ? { kind: "kite", range: near, far: distance + options.playerRange }
      : options.player === "strafe"
        ? { kind: "strafe", range: options.playerRange }
        : { kind: options.player, range: distance };
  return { ...scenario, targets, player: { ...scenario.player, movement: player }, seed: options.seed };
}

export type Placement = "single" | "line" | "row" | "cluster" | "ring" | "boss";

export const placements: { value: Placement; label: string }[] = [
  { value: "single", label: "One dummy" },
  { value: "line", label: "A line, one behind another" },
  { value: "row", label: "A row, side by side" },
  { value: "cluster", label: "A cluster" },
  { value: "ring", label: "A ring around you" },
  { value: "boss", label: "A boss with adds" },
];

export interface PlacementOptions {
  placement: Placement;
  /** Targets in a line, row or ring. */
  count: number;
  /** Tiles between neighbouring targets. */
  spacing: number;
}

/** Lays out targets `distance` tiles ahead, each `radius` tiles round. */
export function layOut(options: PlacementOptions, distance: number, radius: number): Scenario {
  const n = Math.max(1, Math.round(options.count));
  const gap = Math.max(0.1, options.spacing);
  const ahead = (x: number, y: number) => ({ x, y: distance + y, radius });
  switch (options.placement) {
    case "line":
      return { player: { x: 0, y: 0 }, aim: 0, targets: Array.from({ length: n }, (_, i) => ahead(0, i * gap)) };
    case "row":
      return {
        player: { x: 0, y: 0 },
        aim: Math.floor(n / 2),
        targets: Array.from({ length: n }, (_, i) => ahead((i - (n - 1) / 2) * gap, 0)),
      };
    case "cluster": {
      // One in the middle and rings of six around it, hexagon style.
      const targets = [ahead(0, 0)];
      for (let ring = 1; targets.length < n; ring++) {
        for (let k = 0; k < 6 * ring && targets.length < n; k++) {
          const a = (2 * Math.PI * k) / (6 * ring);
          targets.push(ahead(Math.sin(a) * ring * gap, Math.cos(a) * ring * gap));
        }
      }
      return { player: { x: 0, y: 0 }, aim: 0, targets };
    }
    case "ring":
      return {
        player: { x: 0, y: 0 },
        aim: 0,
        targets: Array.from({ length: n }, (_, i) => {
          const a = (2 * Math.PI * i) / n;
          return { x: Math.sin(a) * distance, y: Math.cos(a) * distance, radius };
        }),
      };
    case "boss": {
      // A boss three times the size, adds around it.
      const adds = Array.from({ length: n }, (_, i) => {
        const a = (2 * Math.PI * (i + 0.5)) / n;
        return ahead(Math.sin(a) * (radius * 3 + gap), Math.cos(a) * (radius * 3 + gap));
      });
      return { player: { x: 0, y: 0 }, aim: 0, targets: [{ ...ahead(0, 0), radius: radius * 3 }, ...adds] };
    }
    default:
      return { player: { x: 0, y: 0 }, aim: 0, targets: [ahead(0, 0)] };
  }
}

/** Everything about the simulation's scene that a link keeps. */
export interface SimOptions extends PlacementOptions, MovementOptions {
  fullMp: boolean;
  /** "" for dummies that can't die, "custom" for `customHp`, or a boss's id. */
  hp: string;
  customHp: number;
  respawn: boolean;
  randomRolls: boolean;
  /** Seconds between invulnerable windows, 0 for none, and how long each lasts. */
  invulnerableEvery: number;
  invulnerableFor: number;
  lead: boolean;
}

export const defaultSimOptions: SimOptions = {
  placement: "single",
  count: 5,
  spacing: 1.5,
  targets: "still",
  targetSpeed: 3,
  targetRange: 2,
  player: "still",
  playerRange: 2,
  seed: 1,
  fullMp: true,
  hp: "",
  customHp: 10000,
  respawn: false,
  randomRolls: false,
  invulnerableEvery: 0,
  invulnerableFor: 2,
  lead: false,
};

// Short names in the link. Only options that differ from the defaults are
// written, so a plain build keeps a short link.
const queryKeys: Record<keyof SimOptions, string> = {
  placement: "place",
  count: "n",
  spacing: "gap",
  targets: "tmove",
  targetSpeed: "tspeed",
  targetRange: "trange",
  player: "pmove",
  playerRange: "prange",
  seed: "seed",
  fullMp: "fullmp",
  hp: "hp",
  customHp: "hpn",
  respawn: "respawn",
  randomRolls: "rolls",
  invulnerableEvery: "inv",
  invulnerableFor: "invfor",
  lead: "lead",
};

export function simToQuery(options: SimOptions, q: URLSearchParams): void {
  for (const [key, short] of Object.entries(queryKeys) as [keyof SimOptions, string][]) {
    if (options[key] !== defaultSimOptions[key]) {
      q.set(short, String(options[key]));
    }
  }
}

/** The options in a query; anything missing or invalid is the default. */
export function simFromQuery(query: string): SimOptions {
  const q = new URLSearchParams(query);
  const out: Record<string, unknown> = { ...defaultSimOptions };
  const allowed: Partial<Record<keyof SimOptions, string[]>> = {
    placement: placements.map((p) => p.value),
    targets: targetMovements.map((m) => m.value),
    player: playerMovements.map((m) => m.value),
  };
  for (const [key, short] of Object.entries(queryKeys) as [keyof SimOptions, string][]) {
    const raw = q.get(short);
    const fallback = defaultSimOptions[key];
    if (raw === null) continue;
    if (typeof fallback === "number") {
      const n = Number(raw);
      if (Number.isFinite(n) && n >= 0) out[key] = n;
    } else if (typeof fallback === "boolean") {
      out[key] = raw === "true";
    } else if (!allowed[key] || allowed[key].includes(raw)) {
      out[key] = raw;
    }
  }
  return out as unknown as SimOptions;
}
