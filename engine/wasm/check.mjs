// Runs requests through the WebAssembly engine and prints one response per
// line, the same output as the native rotmg-eval tool.
//
// Usage: node check.mjs <engine.js> <dataset.json> <requests.json>

import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";

const [enginePath, datasetPath, requestsPath] = process.argv.slice(2);
if (!requestsPath) {
  console.error("usage: node check.mjs <engine.js> <dataset.json> <requests.json>");
  process.exit(2);
}

const { default: createEngine } = await import(pathToFileURL(enginePath).href);
const engine = await createEngine();
const api = new engine.Api();
const error = api.load(readFileSync(datasetPath, "utf8"));
if (error) {
  console.error(error);
  process.exit(1);
}
for (const request of JSON.parse(readFileSync(requestsPath, "utf8"))) {
  process.stdout.write(api.evaluate(JSON.stringify(request)) + "\n");
}
api.delete();
