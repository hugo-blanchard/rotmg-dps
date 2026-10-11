// Loads a real dataset into the WebAssembly engine and evaluates every
// weapon once. Fails if the dataset doesn't load or any weapon errors or
// comes out as a non-finite or negative DPS. Run before deploying the site.
//
// Usage: node smoke.mjs <engine.js> <dataset.json>

import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";

const [enginePath, datasetPath] = process.argv.slice(2);
if (!datasetPath) {
  console.error("usage: node smoke.mjs <engine.js> <dataset.json>");
  process.exit(2);
}

const text = readFileSync(datasetPath, "utf8");
const { default: createEngine } = await import(pathToFileURL(enginePath).href);
const api = new (await createEngine()).Api();
const error = api.load(text);
if (error) {
  console.error(`dataset didn't load: ${error}`);
  process.exit(1);
}

const failures = [];
const weapons = JSON.parse(text).weapons;
for (const w of weapons) {
  const res = JSON.parse(api.evaluate(JSON.stringify({ weapon: w.name, attack: 50, dexterity: 50 })));
  if (!res.ok) {
    failures.push(`${w.name}: ${res.error}`);
  } else if (!Number.isFinite(res.breakdown.dps) || res.breakdown.dps < 0) {
    failures.push(`${w.name}: DPS ${res.breakdown.dps}`);
  }
}
api.delete();

if (failures.length) {
  console.error(failures.join("\n"));
  process.exit(1);
}
console.log(`${weapons.length} weapons evaluated`);
