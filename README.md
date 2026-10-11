# rotmg-dps

A DPS calculator for Realm of the Mad God (Exalt). It works out how much damage a build does to a given target, using item data from the game client and formulas sourced from the RealmEye wiki. A small simulation shows the character firing at a dummy so the numbers are easy to check.

Live at https://hugo-blanchard.github.io/rotmg-dps/. It covers weapons for now, including an estimate of how many shots land at a given distance. Abilities and a simulation view are still to come.

## Notice

Unofficial fan tool, not affiliated with DECA. Game assets © DECA Live Operations GmbH.

## How it works

- `engine/` (C++20) does all the math: damage per hit, fire rate, how many shots of a volley land on a round target, and DPS, with the source of every rule it applies. It builds natively for the tests and to WebAssembly for the site.
- `web/` (Svelte and TypeScript) is the site. It runs the WebAssembly engine in a Web Worker.
- Item data comes from the game client. An extractor that isn't part of this repo turns it into a dataset in the format below and publishes it as a `data-*` release, and the site deploys the newest one.

## Building

The engine is C++20 and needs CMake 3.25 or newer. Tests use Catch2, which CMake downloads on the first configure.

```
cmake -S engine -B engine/build
cmake --build engine/build
ctest --test-dir engine/build
```

Pass `-DROTMG_SANITIZE=ON` to build with AddressSanitizer and UBSan (GCC or Clang).

The build also produces `rotmg-dps`, which prints a weapon's DPS against a range of DEF from a dataset file. Run it with no arguments to see the options.

```
engine/build/rotmg-dps dataset.json "Doom Bow" --att 75 --dex 50 --target armored
```

The web app needs Node 24, the WebAssembly engine and a dataset. Build the engine with Emscripten 6.0.12 and copy it into place:

```
emcmake cmake -S engine -B engine/build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build-wasm
mkdir -p web/public/engine
cp engine/build-wasm/engine.js engine/build-wasm/engine.wasm web/public/engine/
```

Then get the newest dataset and start the dev server:

```
gh release download --repo hugo-blanchard/rotmg-dps --pattern dataset.json --dir web/public/data
cd web
npm install
npm run dev
```

`npm run build` writes the static site to `web/dist`.

## Dataset format

The site loads one JSON dataset per game build. It's built from the game client's own item data, the XML stored in `RotMG Exalt_Data/resources.assets`, and published as a release asset. Game data is never committed to this repo.

This section is enough to write your own extractor. Values keep the client's units, and optional fields are left out when the client doesn't set them, so the engine can apply the game's defaults with a source for each.

### Top level

| Field | Meaning | Client source |
|---|---|---|
| `schemaVersion` | Format version. Must equal `kSchemaVersion` in `engine/include/rotmg/schema.hpp` | |
| `build` | Client build the data came from | `build-guid` in `RotMG Exalt_Data/boot.config` |
| `classes` | Player classes, sorted by `type` | `<Object>` elements with `<Player/>` in `players.xml` |
| `weapons` | Weapons, sorted by `type` | object XML files listed under `objects` in the client's `manifest` |

### Classes

| Field | Client source |
|---|---|
| `type` | `type` attribute (hex) |
| `id` | `id` attribute |
| `weaponSlotType` | first value of `<SlotTypes>` |
| `attack`, `dexterity` | `{"base", "max"}` from `<Attack max="60">23</Attack>` and `<Dexterity>` |

### Weapons

An item is a weapon when its `<Class>` is `Equipment`, its `<SlotType>` is some class's weapon slot, and it has at least one `<Projectile>`. Admin-only items, test files (`testing/*.xml`, `equipTest.xml`) and enchantment procs (`equipSubs.xml`) are left out.

Many weapons have Shiny or Retro reskins with the same `<DisplayId>`. A reskin with the same stats as the base item is dropped. One with different stats is kept, with its `id` as its name so names stay unique.

| Field | Client source |
|---|---|
| `type`, `id` | `type` and `id` attributes |
| `name` | `<DisplayId>`, or `id` if there isn't one |
| `slotType`, `tier` | `<SlotType>`, `<Tier>` (no tier for untiered items) |
| `texture` | `{"file", "index"}` from `<Texture>` |
| `statBonuses` | `{"stat", "amount"}` from each `<ActivateOnEquip>IncrementStat</ActivateOnEquip>` |
| `projectiles` | one per `<Projectile>` |
| `subattacks` | one per `<Subattack>` |
| `unsupported` | names of tags on the item that can change its damage but aren't modelled yet, e.g. `OnPlayerShootActivate` or `TurnRate` |

### Projectiles

| Field | Client source |
|---|---|
| `id` | `id` attribute, 0 if missing |
| `objectId` | `<ObjectId>`, the projectile's sprite object |
| `speed` | `<Speed>`, in tenths of a tile per second |
| `minDamage`, `maxDamage` | `<MinDamage>`, `<MaxDamage>` |
| `lifetimeMs` | `<LifetimeMS>` |
| `armorPiercing`, `multiHit`, `boomerang`, `wavy`, `parametric` | `true` when the empty element is present |
| `size`, `amplitude`, `frequency`, `magnitude`, `acceleration`, `accelerationDelay`, `speedClamp` | the element of the same name |
| `conditionEffects` | `{"effect", "duration"}` from `<ConditionEffect duration="3">Stunned</ConditionEffect>` |

Some weapons carry placeholder projectiles with zero speed, damage and lifetime. They're kept as they are.

### Subattacks

Older weapons have no `<Subattack>` and set their firing values on the item itself. Those get a single subattack built from the item. In newer weapons, a subattack that leaves a value out takes the item's value.

| Field | Client source |
|---|---|
| `projectileId` | `projectileId` attribute, or the first projectile for older weapons |
| `numProjectiles` | `<NumProjectiles>`, 1 if missing. Can be 0 for slots an upgrade fills in |
| `rateOfFire` | `<RateOfFire>`, 1 if missing |
| `arcGap`, `defaultAngle` | `<ArcGap>`, `<DefaultAngle>` (sometimes spelled `defaultAngle`), in degrees |
| `posOffset` | `[x, y]` from `<PosOffset>-.5,0</PosOffset>` |
| `burstCount`, `burstDelay`, `burstMinDelay` | the element of the same name |

### Example

Made-up values, trimmed to one class and one weapon:

```json
{
  "schemaVersion": 1,
  "build": "0123456789abcdef0123456789abcdef",
  "classes": [
    { "type": 768, "id": "Example Class", "weaponSlotType": 1,
      "attack": { "base": 15, "max": 75 }, "dexterity": { "base": 15, "max": 50 } }
  ],
  "weapons": [
    { "type": 4097, "id": "Example Sword", "name": "Example Sword",
      "slotType": 1, "tier": 5, "texture": { "file": "exampleSheet", "index": 16 },
      "statBonuses": [ { "stat": "DEX", "amount": 4 } ],
      "projectiles": [
        { "id": 0, "objectId": "Example Bolt", "speed": 100,
          "minDamage": 50, "maxDamage": 80, "lifetimeMs": 350 }
      ],
      "subattacks": [ { "projectileId": 0, "numProjectiles": 1, "rateOfFire": 1 } ] }
  ]
}
```

## Deploying

Pushing to `main`, or publishing a new `data-*` release, runs `.github/workflows/pages.yml`. It builds the engine and the site, downloads the newest dataset, checks that every weapon in it evaluates and that the hit estimate matches what the wiki says about tiered bows, longbows and the Torrential Staff, and deploys to GitHub Pages.

## License

AGPL-3.0. See [LICENSE](LICENSE).
