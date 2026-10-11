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
  curve?: { from: number; to: number; step: number };
}

export type Source = "wiki" | "client-data" | "user" | "assumed";

export interface Term {
  name: string;
  value: number;
  source: Source;
  reference: string;
}

export interface SubattackResult {
  projectileId: number;
  volleysPerSecond: number;
  hitsPerVolley: number;
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

export type EvaluateResponse =
  | { ok: true; breakdown: Breakdown; curve?: CurvePoint[] }
  | { ok: false; error: string };

export type WorkerRequest =
  | { id: number; kind: "load"; dataset: string }
  | { id: number; kind: "evaluate"; request: string };

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

  private send(message: { kind: "load"; dataset: string } | { kind: "evaluate"; request: string }) {
    const id = this.nextId++;
    return new Promise<WorkerResponse>((resolve) => {
      this.pending.set(id, resolve);
      this.worker.postMessage({ id, ...message } satisfies WorkerRequest);
    });
  }
}
