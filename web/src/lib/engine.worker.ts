// Loads engine.js and engine.wasm, built from engine/ with Emscripten and
// deployed under engine/ next to the site, and answers Engine's messages.

import type { WorkerRequest, WorkerResponse } from "./engine";

interface Api {
  load(datasetJson: string): string;
  evaluate(requestJson: string): string;
  startSimulation(requestJson: string): string;
  advanceSimulation(ms: number): string;
  simulateMany(requestJson: string): string;
  optimizeWithProgress(requestJson: string, report: (progress: string) => void): string;
  estimateOptimize(requestJson: string): string;
  evaluateChoices(requestJson: string): string;
}

// The worker global isn't in the DOM typings this project compiles against.
const scope = self as unknown as {
  onmessage: ((e: MessageEvent<WorkerRequest>) => void) | null;
  postMessage(message: WorkerResponse): void;
};

const api: Promise<Api> = (async () => {
  // A full URL keeps Vite from trying to bundle a file it only serves.
  const url = new URL(`${import.meta.env.BASE_URL}engine/engine.js`, self.location.origin).href;
  const { default: createEngine } = await import(/* @vite-ignore */ url);
  const engine = await createEngine();
  return new engine.Api() as Api;
})();

scope.onmessage = async (e) => {
  const message = e.data;
  try {
    const engine = await api;
    switch (message.kind) {
      case "load":
      case "simStart": {
        const error =
          message.kind === "load" ? engine.load(message.dataset) : engine.startSimulation(message.request);
        scope.postMessage(error ? { id: message.id, error } : { id: message.id });
        break;
      }
      case "evaluate":
        scope.postMessage({ id: message.id, result: engine.evaluate(message.request) });
        break;
      case "simAdvance":
        scope.postMessage({ id: message.id, result: engine.advanceSimulation(message.ms) });
        break;
      case "simMany":
        scope.postMessage({ id: message.id, result: engine.simulateMany(message.request) });
        break;
      case "optimize": {
        const report = (progress: string) => scope.postMessage({ id: message.id, progress });
        scope.postMessage({ id: message.id, result: engine.optimizeWithProgress(message.request, report) });
        break;
      }
      case "choices":
        scope.postMessage({ id: message.id, result: engine.evaluateChoices(message.request) });
        break;
      case "estimate":
        scope.postMessage({ id: message.id, result: engine.estimateOptimize(message.request) });
        break;
    }
  } catch (err) {
    scope.postMessage({ id: message.id, error: `engine failed to start: ${String(err)}` });
  }
};
