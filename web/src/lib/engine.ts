// Runs the WebAssembly engine in a Web Worker so evaluating never blocks the
// page. Request and response shapes mirror engine/src/api.cpp.

import type { Scenario } from "./scenario";

export interface PlayerStatus {
  weak: boolean;
  damaging: boolean;
  berserk: boolean;
  dazed: boolean;
}

export interface Target {
  defense: number;
  armored: boolean;
  armorBroken: boolean;
  exposed: boolean;
  cursed: boolean;
}

export interface GearEnchantments {
  ring: string[];
  armor: string[];
  ability: string[];
}

export interface EvaluateRequest {
  weapon: string;
  ability?: string;
  attack: number;
  dexterity: number;
  wisdom?: number;
  vitality?: number;
  defense?: number;
  speed?: number;
  maxHitPoints?: number;
  maxMagicPoints?: number;
  status: PlayerStatus;
  target: Target;
  /** The armor and ring worn, by name. */
  armor?: string;
  ring?: string;
  /** Item types of other gear worn, when it isn't named as the armor or ring. */
  wearing?: number[];
  /** Ids of up to 4 enchantments on the weapon. */
  enchantments?: string[];
  /** Up to 4 enchantment ids each on the other gear. */
  gearEnchantments?: GearEnchantments;
  /** Class id, which relative enchantments measure bonus stats against. */
  class?: string;
  /** Where the simulation's targets stand; one ahead at the hit model's distance without it. */
  scenario?: Scenario;
  hitsPerVolley?: (number | null)[];
  curatedHitsPerVolley?: (number | null)[];
  hitModel?: { distance: number; radius: number };
  curve?: { from: number; to: number; step: number };
  hitCurve?: { from: number; to: number; step: number };
}

export type Source = "wiki" | "client-data" | "geometric-estimate" | "curated" | "user" | "assumed";

export interface Term {
  name: string;
  value: number;
  source: Source;
  reference: string;
}

export interface SubattackResult {
  projectileId: number;
  projectilesPerVolley: number;
  volleysPerSecond: number;
  hitsPerVolley: number;
  hitsSource: Source;
  damagePerHit: number;
  dps: number;
}

// Damage from an on-shoot effect, like a poison burst every few seconds.
export interface ProcResult {
  name: string;
  perSecond: number;
  damage: number;
  dps: number;
  reference: string;
}

export interface Breakdown {
  dps: number;
  subattacks: SubattackResult[];
  procs: ProcResult[];
  terms: Term[];
  assumptions: string[];
  unsupported: string[];
}

export interface Buff {
  effect: string;
  on: "player" | "target";
  amount: number;
  durationS: number;
  /** A damage boost's multiplier on damage taken; 1 for anything else. */
  factor: number;
}

export interface AbilityBreakdown {
  mpCost: number;
  cooldownS: number;
  mpPerSecond: number;
  castsPerSecond: number;
  damagePerCast: number;
  dps: number;
  parts: { name: string; damage: number; reference: string }[];
  buffs: Buff[];
  terms: Term[];
  assumptions: string[];
  unsupported: string[];
}

export interface BuildBreakdown {
  dps: number;
  weaponDps: number;
  uptimes: (Buff & { share: number })[];
  assumptions: string[];
}

export interface CurvePoint {
  defense: number;
  dps: number;
}

export interface HitCurvePoint {
  distance: number;
  hits: number;
}

// A set some worn items belong to: how many slots hold its pieces and the
// stats that adds, every bonus for that many pieces added up.
export interface WornSet {
  type: number;
  name: string;
  pieces: number;
  stats: { stat: string; amount: number }[];
  unsupported: string[];
}

export type EvaluateResponse =
  | {
      ok: true;
      breakdown: Breakdown;
      build: BuildBreakdown;
      ability?: AbilityBreakdown;
      sets?: WornSet[];
      enchantmentStats?: { stat: string; amount: number }[];
      /** Stats the items' relative on-equip effects add, like a share of one stat as another. */
      itemStats?: { stat: string; amount: number }[];
      curve?: CurvePoint[];
      hitCurve?: HitCurvePoint[];
      hitModelGaps?: string[];
    }
  | { ok: false; error: string };

export interface SimShot {
  /** -1 for the ability's and procs' shots. */
  subattack: number;
  /** A proc is one of the weapon's on-shoot effects. */
  from: "weapon" | "ability" | "proc";
  projectileId: number;
  objectId: string;
  x: number;
  y: number;
  heading: number;
  ageMs: number;
  hit: boolean;
}

export interface SimCounters {
  volleys: number;
  projectiles: number;
  hits: number;
  damage: number;
  resolvedVolleys: number;
  resolvedHits: number;
}

export interface SimTarget {
  x: number;
  y: number;
  radius: number;
  defense: number;
  hits: number;
  /** All damage taken, the ability's included. */
  damage: number;
  abilityDamage: number;
  statuses: string[];
  /** Hit points left, for a target that can die. */
  hp: number;
  kills: number;
  /** When it first died. */
  killedMs: number | null;
  /** How long each debuff has been up on it. */
  uptimeMs: Record<string, number>;
  invulnerable: boolean;
}

export interface SimEffect {
  kind: "poison" | "trap" | "blast" | "bolt";
  x: number;
  y: number;
  radius: number;
  /** Traps: waiting for a target. */
  armed: boolean;
  /** Lightning: from the player through every target it struck. */
  chain: { x: number; y: number }[];
}

/** One simulation's state; see AdvanceSimulation in engine/src/api.cpp. */
export interface SimRun {
  timeMs: number;
  player: { x: number; y: number };
  /** Tiles per second, from SPD. */
  playerSpeed: number;
  /** Index of the target the player aims at. */
  aim: number;
  target: { x: number; y: number; radius: number };
  targets: SimTarget[];
  shots: SimShot[];
  totals: SimCounters;
  perSubattack: SimCounters[];
  mp: number;
  mpMax: number;
  /** Statuses and stat boosts on the player now, like "Berserk". */
  statuses: string[];
  /** How long each status or stat boost has been up on the player. */
  uptimeMs: Record<string, number>;
  ability: { casts: number; hits: number; damage: number };
  procs: { activations: number; hits: number; damage: number };
  /** Hits since the last state, for damage numbers. */
  hits: { target: number; damage: number; from: "weapon" | "ability" | "proc" }[];
  /** Damage done in each bucket and MP at its end, for buckets since the last state. */
  history: { bucketMs: number; from: number; damage: number[]; mp: number[] };
  effects: SimEffect[];
}

/** How one build did over many seeded runs; lists are sorted. */
export interface Spread {
  runs: number;
  /** DPS counted up to the last kill. */
  dps: number[];
  /** Seconds, for the runs that killed every target. */
  timeToKill: number[];
  unfinished: number;
}

export type SpreadResponse = (Spread & { ok: true; compare?: Spread }) | { ok: false; error: string };

/** What the optimizer may change; see Api::Optimize in engine/src/api.cpp. */
export interface OptimizeOptions {
  /** Slots whose items stay as they are. */
  lock: ("weapon" | "ability" | "armor" | "ring")[];
  /** Which items may be tried: tiered ones, set pieces, and the rest. */
  pools: ("tiered" | "untiered" | "set")[];
  enchantments: "keep" | "none" | "best";
  /** Whether to use enchantments only an artifact or engraving puts on an item. */
  artifacts?: boolean;
  /** How many enchantments to fill on each item, 2 unless set. */
  slots?: number;
  /**
   * Which stats the search starts again from the ability that scales best
   * with: every one (the default), none, or one, like "DEF".
   */
  around?: string;
  /** What the weapon's DPS, with the ability's buffs, and the ability's own count for; 1 each unless set. */
  weights?: { weapon: number; ability: number };
  results?: number;
}

export interface OptimizedBuild {
  /** What builds are ranked by: the weighted weapon and ability DPS. */
  score: number;
  dps: number;
  weaponDps: number;
  abilityDps: number;
  weapon: string;
  ability: string;
  armor: string;
  ring: string;
  enchantments: Record<"weapon" | "ability" | "armor" | "ring", string[]>;
}

/** Where a search stands: how far along it is, from 0 to 1, a rough guess, and the best builds so far. */
export interface OptimizeProgress {
  done: number;
  baseline: number;
  evaluated: number;
  builds: OptimizedBuild[];
}

export type OptimizeResponse =
  | { ok: true; baseline: number; evaluated: number; builds: OptimizedBuild[] }
  | { ok: false; error: string };

export type SimState =
  | (SimRun & {
      ok: true;
      /** The build compared with, in the same scene, without what's only drawn. */
      compare?: Omit<SimRun, "shots" | "effects" | "hits">;
    })
  | { ok: false; error: string };

type Message =
  | { kind: "load"; dataset: string }
  | { kind: "evaluate"; request: string }
  | { kind: "simStart"; request: string }
  | { kind: "simAdvance"; ms: number }
  | { kind: "simMany"; request: string }
  | { kind: "optimize"; request: string };

export type WorkerRequest = Message & { id: number };

export interface WorkerResponse {
  id: number;
  error?: string;
  result?: string;
  /** A report while the request runs; the result comes later. */
  progress?: string;
}

// One worker with the dataset loaded, and the requests waiting on it.
class Channel {
  private worker = new Worker(new URL("./engine.worker.ts", import.meta.url), { type: "module" });
  private pending = new Map<number, { resolve: (r: WorkerResponse) => void; progress?: (p: string) => void }>();
  private nextId = 1;

  private constructor() {
    this.worker.onmessage = (e: MessageEvent<WorkerResponse>) => {
      const waiting = this.pending.get(e.data.id);
      if (e.data.progress !== undefined) {
        waiting?.progress?.(e.data.progress);
        return;
      }
      waiting?.resolve(e.data);
      this.pending.delete(e.data.id);
    };
  }

  static async start(datasetText: string): Promise<Channel> {
    const channel = new Channel();
    const res = await channel.send({ kind: "load", dataset: datasetText });
    if (res.error) {
      channel.worker.terminate();
      throw new Error(res.error);
    }
    return channel;
  }

  send(message: Message, progress?: (p: string) => void) {
    const id = this.nextId++;
    return new Promise<WorkerResponse>((resolve) => {
      this.pending.set(id, { resolve, progress });
      this.worker.postMessage({ id, ...message } satisfies WorkerRequest);
    });
  }

  /** Ends the worker mid-request; whatever was waiting gets the error "stopped". */
  stop() {
    this.worker.terminate();
    for (const [id, waiting] of this.pending) {
      waiting.resolve({ id, error: "stopped" });
    }
    this.pending.clear();
  }
}

export class Engine {
  // Searches run on a worker of their own, so the rest of the page keeps
  // working meanwhile and a search can be stopped by ending its worker.
  private optimizer: Promise<Channel> | null = null;

  private constructor(
    private main: Channel,
    private datasetText: string,
  ) {}

  /** Starts the worker and loads the dataset into the engine. */
  static async start(datasetText: string): Promise<Engine> {
    return new Engine(await Channel.start(datasetText), datasetText);
  }

  async evaluate(request: EvaluateRequest): Promise<EvaluateResponse> {
    const res = await this.send({ kind: "evaluate", request: JSON.stringify(request) });
    if (res.error) {
      return { ok: false, error: res.error };
    }
    return JSON.parse(res.result!) as EvaluateResponse;
  }

  /** Starts a simulation, replacing any running one. Resolves to an error message, or "". */
  async startSimulation(request: EvaluateRequest): Promise<string> {
    const res = await this.send({ kind: "simStart", request: JSON.stringify(request) });
    return res.error ?? "";
  }

  async advanceSimulation(ms: number): Promise<SimState> {
    const res = await this.send({ kind: "simAdvance", ms });
    if (res.error) {
      return { ok: false, error: res.error };
    }
    return JSON.parse(res.result!) as SimState;
  }

  /**
   * Looks for the build with the most DPS; see Api::Optimize. Resolves to
   * null when stopped, and the last progress report then has the best
   * builds found.
   */
  async optimize(
    request: EvaluateRequest & { optimize: OptimizeOptions },
    onProgress?: (p: OptimizeProgress) => void,
  ): Promise<OptimizeResponse | null> {
    let channel: Channel;
    try {
      this.optimizer ??= Channel.start(this.datasetText);
      channel = await this.optimizer;
    } catch (err) {
      this.optimizer = null;
      return { ok: false, error: String(err) };
    }
    const report = (p: string) => onProgress?.(JSON.parse(p) as OptimizeProgress);
    const res = await channel.send({ kind: "optimize", request: JSON.stringify(request) }, report);
    if (res.error === "stopped") {
      return null;
    }
    if (res.error) {
      return { ok: false, error: res.error };
    }
    return JSON.parse(res.result!) as OptimizeResponse;
  }

  /** Stops a running search; its worker starts again for the next one. */
  stopOptimizing() {
    void this.optimizer?.then((c) => c.stop());
    this.optimizer = null;
  }

  /** Runs the request many times with random rolls; see Api::SimulateMany. */
  async simulateMany(request: EvaluateRequest & { runs: number; seconds: number }): Promise<SpreadResponse> {
    const res = await this.send({ kind: "simMany", request: JSON.stringify(request) });
    if (res.error) {
      return { ok: false, error: res.error };
    }
    return JSON.parse(res.result!) as SpreadResponse;
  }

  private send(message: Message) {
    return this.main.send(message);
  }
}
