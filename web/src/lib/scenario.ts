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
}

export interface Scenario {
  player: { x: number; y: number; movement?: Movement };
  /** Index of the target the player aims at. */
  aim: number;
  targets: ScenarioTarget[];
  seed?: number;
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
