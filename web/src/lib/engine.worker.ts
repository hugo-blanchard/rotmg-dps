// Loads engine.js and engine.wasm, built from engine/ with Emscripten and
// deployed under engine/ next to the site, and answers Engine's messages.

import type { WorkerRequest, WorkerResponse } from "./engine";

interface Api {
  load(datasetJson: string): string;
  evaluate(requestJson: string): string;
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
    if (message.kind === "load") {
      const error = engine.load(message.dataset);
      scope.postMessage(error ? { id: message.id, error } : { id: message.id });
    } else {
      scope.postMessage({ id: message.id, result: engine.evaluate(message.request) });
    }
  } catch (err) {
    scope.postMessage({ id: message.id, error: `engine failed to start: ${String(err)}` });
  }
};
