// Where the simulation's targets stand. The engine takes plain positions in
// tiles, with the player at the origin facing +y; these presets lay them
// out around the distance the hit estimate uses.

export interface ScenarioTarget {
  x: number;
  y: number;
  radius?: number;
}

export interface Scenario {
  player: { x: number; y: number };
  /** Index of the target the player aims at. */
  aim: number;
  targets: ScenarioTarget[];
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
