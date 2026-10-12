// Loads a real dataset into the WebAssembly engine and checks it before the
// site deploys it:
//
// - every weapon evaluates to a finite, non-negative DPS
// - every ability evaluates and simulates
// - every armor and ring evaluates
// - the optimizer works for every class
// - the hit estimate agrees with what the wiki documents about real weapons
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

const evaluate = (request) => JSON.parse(api.evaluate(JSON.stringify(request)));
const failures = [];
const weapons = JSON.parse(text).weapons;

for (const w of weapons) {
  const res = evaluate({ weapon: w.name, attack: 50, dexterity: 50, hitModel: { distance: 4 } });
  if (!res.ok) {
    failures.push(`${w.name}: ${res.error}`);
  } else if (!Number.isFinite(res.breakdown.dps) || res.breakdown.dps < 0) {
    failures.push(`${w.name}: DPS ${res.breakdown.dps}`);
  }
}

// Total hits per volley at each distance from `from` to `to`.
function hitCurve(name, from, to, step) {
  const res = evaluate({ weapon: name, hitCurve: { from, to, step } });
  if (!res.ok) {
    failures.push(`${name}: ${res.error}`);
    return [];
  }
  return res.hitCurve;
}

function find(name) {
  const w = weapons.find((x) => x.name === name);
  if (!w) {
    failures.push(`${name} is missing from the dataset; update the checks in smoke.mjs`);
  }
  return w;
}

// Weapon Attributes: a spread's side shots stop hitting at its "true range",
// where the target's 0.5 tile radius no longer covers the angle. Today's
// tiered bows fire a center shot and a pair 14 degrees apart.
for (const w of weapons.filter((x) => x.slotType === 3 && x.tier !== undefined && !x.name.includes("Longbow"))) {
  const spread = w.subattacks.filter((s) => s.numProjectiles > 1 && !s.posOffset);
  if (spread.length === 0) continue;
  const halfAngle = Math.max(
    ...spread.map((s) => ((s.numProjectiles - 1) / 2) * (s.arcGap ?? 11.25) + Math.abs(s.defaultAngle ?? 0)),
  );
  const trueRange = 0.5 / Math.sin((halfAngle * Math.PI) / 180);
  const total = w.subattacks.reduce((n, s) => n + s.numProjectiles, 0);
  const [inside] = hitCurve(w.name, trueRange - 0.15, trueRange - 0.15, 1);
  const [outside] = hitCurve(w.name, trueRange + 0.15, trueRange + 0.15, 1);
  if (inside?.hits !== total || !(outside?.hits < total)) {
    failures.push(
      `${w.name}: expected all ${total} shots to hit inside ${trueRange.toFixed(2)} tiles and fewer outside, ` +
        `got ${inside?.hits} and ${outside?.hits}`,
    );
  }
}

// Torrential Staff: "practically impossible to land more than 2" of its 5 shots.
if (find("Torrential Staff")) {
  const most = Math.max(...hitCurve("Torrential Staff", 0.5, 10, 0.25).map((p) => p.hits));
  if (most > 2) {
    failures.push(`Torrential Staff: ${most} hits per volley at some distance, the wiki says at most 2`);
  }
}

// Earthen Ward: its shots circle out to a ring, and "it is technically
// possible to hit at least two shots at full range", but all three only
// when "sitting on top of enemies". Datasets from before circleTurnDelay
// existed fire it in straight lines.
if (find("Earthen Ward")?.projectiles.some((p) => p.circleTurnDelay !== undefined)) {
  const [atRing] = hitCurve("Earthen Ward", 6, 6, 1);
  const most = Math.max(...hitCurve("Earthen Ward", 1, 7, 0.25).map((p) => p.hits));
  if (!(atRing?.hits >= 2) || most > 2) {
    failures.push(`Earthen Ward: ${atRing?.hits} hits at 6 tiles and at most ${most} from 1 tile out, the wiki says 2 and 2`);
  }
}

// Longbows: "more accurate as you get further away, as their shots start
// offset but converge". Hits must never drop as the target moves away,
// from 1 tile (any closer and the target overlaps where shots spawn) to
// half a tile short of their 7.04 tile range.
for (const w of weapons.filter((x) => x.slotType === 3 && (x.name.includes("Longbow") || x.name === "Hama Yumi"))) {
  const curve = hitCurve(w.name, 1, 6.5, 0.25);
  for (let i = 1; i < curve.length; i++) {
    if (curve[i].hits < curve[i - 1].hits) {
      failures.push(`${w.name}: hits drop from ${curve[i - 1].hits} to ${curve[i].hits} at ${curve[i].distance} tiles`);
      break;
    }
  }
}
find("Hama Yumi");

// Every ability evaluates with a weapon of its class, and the build adds up:
// weapon (with buffs) plus ability, never below the weapon on its own.
const { classes, abilities = [] } = JSON.parse(text);
let abilityBuilds = 0;
for (const a of abilities) {
  const cls = classes.find((c) => c.abilitySlotType === a.slotType);
  const weapon = cls && weapons.find((w) => w.slotType === cls.weaponSlotType);
  if (!weapon) {
    failures.push(`${a.name}: no class and weapon to evaluate it with`);
    continue;
  }
  const res = evaluate({
    weapon: weapon.name,
    ability: a.name,
    attack: cls.attack.max,
    dexterity: cls.dexterity.max,
    wisdom: cls.wisdom?.max ?? 0,
    hitModel: { distance: 4 },
    target: { defense: 20 },
  });
  if (!res.ok) {
    failures.push(`${a.name}: ${res.error}`);
    continue;
  }
  const { dps, weaponDps } = res.build;
  if (![dps, weaponDps, res.ability.dps].every(Number.isFinite)) {
    failures.push(`${a.name}: DPS ${dps}, weapon ${weaponDps}, ability ${res.ability.dps}`);
  } else if (Math.abs(dps - weaponDps - res.ability.dps) > 1e-6 * Math.max(1, dps)) {
    failures.push(`${a.name}: build ${dps} isn't weapon ${weaponDps} plus ability ${res.ability.dps}`);
  }
  // And casts in the simulation without breaking anything.
  const error = api.startSimulation(
    JSON.stringify({ weapon: weapon.name, ability: a.name, wisdom: cls.wisdom?.max ?? 0, hitModel: { distance: 4 } }),
  );
  const state = error ? null : JSON.parse(api.advanceSimulation(30_000));
  if (!state?.ok) {
    failures.push(`${a.name}: simulation failed: ${error || state?.error}`);
  } else if (!(state.ability.casts > 0) || !Number.isFinite(state.ability.damage) || !Number.isFinite(state.mp)) {
    failures.push(`${a.name}: simulated ${state.ability.casts} casts, ${state.ability.damage} damage`);
  }
  abilityBuilds++;
}

// Every armor and ring evaluates when worn by a class that can wear it.
const { armors = [], rings = [] } = JSON.parse(text);
let gearWorn = 0;
for (const [kind, list] of [["armor", armors], ["ring", rings]]) {
  for (const g of list) {
    const cls = classes.find((c) => (kind === "armor" ? c.armorSlotType : c.ringSlotType) === g.slotType);
    const weapon = cls && weapons.find((w) => w.slotType === cls.weaponSlotType);
    if (!weapon) {
      failures.push(`${g.name}: no class wears it`);
      continue;
    }
    const res = evaluate({ weapon: weapon.name, [kind]: g.name, attack: cls.attack.max, dexterity: cls.dexterity.max });
    if (!res.ok || !Number.isFinite(res.build.dps)) {
      failures.push(`${g.name}: ${res.ok ? `DPS ${res.build.dps}` : res.error}`);
    }
    gearWorn++;
  }
}

// The optimizer runs for every class and never ends below where it started.
let optimized = 0;
for (const cls of classes) {
  const weapon = weapons.find((w) => w.slotType === cls.weaponSlotType);
  if (!weapon) continue;
  const res = JSON.parse(
    api.optimize(
      JSON.stringify({
        class: cls.id,
        weapon: weapon.name,
        attack: cls.attack.max,
        dexterity: cls.dexterity.max,
        wisdom: cls.wisdom?.max ?? 0,
        hitModel: { distance: 4 },
        optimize: { enchantments: "none" },
      }),
    ),
  );
  if (!res.ok || !(res.builds[0]?.dps >= res.baseline)) {
    failures.push(`optimizing ${cls.id}: ${res.ok ? `${res.builds[0]?.dps} below ${res.baseline}` : res.error}`);
  }
  optimized++;
}

// Every enchantment that changes a weapon, rather than only adding stats,
// evaluates on the first weapon it fits. Ones for other gear are skipped.
const { enchantments = [] } = JSON.parse(text);
const fits = (e, w) =>
  e.compatible.every((l) => (w.labels ?? []).includes(l)) &&
  !(e.incompatible ?? []).some((l) => (w.labels ?? []).includes(l)) &&
  !(e.incompatibleItems ?? []).includes(w.id);
let enchanted = 0;
for (const e of enchantments) {
  if ((e.mutators ?? []).every((m) => m.tag === "ActivateOnEquip")) continue;
  const weapon = weapons.find((w) => fits(e, w));
  if (!weapon) continue;
  const res = evaluate({ weapon: weapon.name, enchantments: [e.id], attack: 50, dexterity: 50, hitModel: { distance: 4 } });
  if (!res.ok) {
    failures.push(`${e.name} on ${weapon.name}: ${res.error}`);
  } else if (!Number.isFinite(res.build.dps) || res.build.dps < 0) {
    failures.push(`${e.name} on ${weapon.name}: DPS ${res.build.dps}`);
  }
  enchanted++;
}

// The simulation view must agree with the numbers next to it: once every
// shot of a volley has landed or expired, it must have counted exactly the
// hits the estimate predicts.
let simulated = 0;
for (const w of weapons) {
  for (const distance of [2, 5]) {
    const request = { weapon: w.name, attack: 50, dexterity: 50, hitModel: { distance, radius: 0.5 } };
    const estimate = evaluate(request);
    if (!estimate.ok || estimate.breakdown.subattacks.some((s) => s.hitsSource !== "geometric-estimate")) {
      continue;
    }
    const error = api.startSimulation(JSON.stringify(request));
    if (error) {
      failures.push(`${w.name}: simulation didn't start: ${error}`);
      continue;
    }
    const state = JSON.parse(api.advanceSimulation(10_000));
    state.perSubattack.forEach((c, i) => {
      const expected = estimate.breakdown.subattacks[i].hitsPerVolley * c.resolvedVolleys;
      if (c.resolvedHits !== expected) {
        failures.push(
          `${w.name} at ${distance} tiles, subattack ${i + 1}: simulated ${c.resolvedHits} hits ` +
            `in ${c.resolvedVolleys} volleys, the estimate gives ${expected}`,
        );
      }
    });
    simulated++;
  }
}

api.delete();

if (failures.length) {
  console.error(failures.join("\n"));
  process.exit(1);
}
console.log(
  `${weapons.length} weapons, ${abilityBuilds} abilities, ${gearWorn} armors and rings and ${enchanted} weapon ` +
    `enchantments evaluated, ` +
    `hit estimate checks passed, ${optimized} classes optimized, ` +
    `${simulated} simulations agree with the estimate`,
);
