// Runs the WebAssembly engine in a Web Worker so evaluating never blocks the
// page. Request and response shapes mirror engine/src/api.cpp.

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

export interface EvaluateRequest {
  weapon: string;
  attack: number;
  dexterity: number;
  status: PlayerStatus;
  target: Target;
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

export interface Breakdown {
  dps: number;
  subattacks: SubattackResult[];
  terms: Term[];
  assumptions: string[];
  unsupported: string[];
}

export interface CurvePoint {
  defense: number;
  dps: number;
}

export interface HitCurvePoint {
  distance: number;
  hits: number;
}

export type EvaluateResponse =
  | {
      ok: true;
      breakdown: Breakdown;
      curve?: CurvePoint[];
      hitCurve?: HitCurvePoint[];
      hitModelGaps?: string[];
    }
  | { ok: false; error: string };

export interface SimShot {
  subattack: number;
  projectileId: number;
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

export type SimState =
  | {
      ok: true;
      timeMs: number;
      target: { x: number; y: number; radius: number };
      shots: SimShot[];
      totals: SimCounters;
      perSubattack: SimCounters[];
    }
  | { ok: false; error: string };

type Message =
  | { kind: "load"; dataset: string }
  | { kind: "evaluate"; request: string }
  | { kind: "simStart"; request: string }
  | { kind: "simAdvance"; ms: number };

export type WorkerRequest = Message & { id: number };

export interface WorkerResponse {
  id: number;
  error?: string;
  result?: string;
}

export class Engine {
  private worker = new Worker(new URL("./engine.worker.ts", import.meta.url), { type: "module" });
  private pending = new Map<number, (r: WorkerResponse) => void>();
  private nextId = 1;

  private constructor() {
    this.worker.onmessage = (e: MessageEvent<WorkerResponse>) => {
      this.pending.get(e.data.id)?.(e.data);
      this.pending.delete(e.data.id);
    };
  }

  /** Starts the worker and loads the dataset into the engine. */
  static async start(datasetText: string): Promise<Engine> {
    const engine = new Engine();
    const res = await engine.send({ kind: "load", dataset: datasetText });
    if (res.error) {
      engine.worker.terminate();
      throw new Error(res.error);
    }
    return engine;
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

  private send(message: Message) {
    const id = this.nextId++;
    return new Promise<WorkerResponse>((resolve) => {
      this.pending.set(id, resolve);
      this.worker.postMessage({ id, ...message } satisfies WorkerRequest);
    });
  }
}
