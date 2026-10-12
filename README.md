# rotmg-dps

A DPS calculator for Realm of the Mad God (Exalt). It works out how much damage a build does to a given target, using item data from the game client and formulas sourced from the RealmEye wiki. A simulation plays the fight out step by step, so the numbers are easy to check and you can see what they leave out: groups of targets, targets and a player that move, the opening burst of a full MP bar, and how long a boss takes to die.

Live at https://hugo-blanchard.github.io/rotmg-dps/. It covers weapons, including an estimate of how many shots land at a given distance and the effects they trigger on shoot; abilities, with their own damage and the buffs and debuffs they keep up; set bonuses; and enchantments on the weapon. Targets can be any DEF or one of the game's bosses. In the simulation you place one or more dummies, make them and yourself move, give them HP, and pin a build to compare with another in the same scene. Effects it can't model yet, like summons and hexes, are named on the page.

## Notice

Unofficial fan tool, not affiliated with DECA. Game assets © DECA Live Operations GmbH.

## How it works

- `engine/` (C++20) does all the math: damage per hit, fire rate, how many shots of a volley land on a round target, on-shoot effects, ability damage, MP and buff uptime, set bonuses, enchantments and DPS, with the source of every rule it applies. It also runs the step-by-step simulation the site animates: projectiles move with the same code as the hit estimate, against any number of round targets that can move and die, while the ability is cast whenever MP allows and the weapon's on-shoot effects go off. With one still target it agrees with the calculated numbers. It builds natively for the tests and to WebAssembly for the site.
- `web/` (Svelte and TypeScript) is the site. It runs the WebAssembly engine in a Web Worker.
- Item data and sprites come from the game client. An extractor that isn't part of this repo turns them into the files described below and publishes them as a `data-*` release, and the site deploys the newest one.

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

Then get the newest dataset and sprites and start the dev server:

```
gh release download --repo hugo-blanchard/rotmg-dps --pattern dataset.json --pattern 'sprites.*' --pattern status.json --dir web/public/data
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
| `abilities` | Abilities, sorted by `type`. Missing from datasets made before abilities were added | the same object XML files |
| `armors`, `rings` | Armor and rings, each sorted by `type`. Missing from datasets made before they were added | the same object XML files |
| `targets` | Bosses the site offers as targets. Optional | the same object XML files |
| `sets` | Equipment sets with a piece in the dataset, sorted by `type`. Optional | `equipmentsets.xml` |
| `enchantments` | Enchantments that fit at least one weapon in the dataset, or any ring, armor or ability, sorted by `type`. Optional | `enchantments.xml` |

### Classes

| Field | Client source |
|---|---|
| `type` | `type` attribute (hex) |
| `id` | `id` attribute |
| `weaponSlotType` | first value of `<SlotTypes>` |
| `attack`, `dexterity` | `{"base", "max"}` from `<Attack max="60">23</Attack>` and `<Dexterity>` |
| `abilitySlotType` | second value of `<SlotTypes>` |
| `armorSlotType`, `ringSlotType` | third and fourth values of `<SlotTypes>`; missing from datasets made before armor and rings were added |
| `maxHitPoints`, `maxMagicPoints`, `defense`, `speed`, `vitality`, `wisdom` | `{"base", "max"}` from `<MaxHitPoints>`, `<MaxMagicPoints>`, `<Defense>`, `<Speed>`, `<HpRegen>` and `<MpRegen>` |

The fields from `abilitySlotType` down were added with abilities and are missing from older datasets.

### Weapons

An item is a weapon when its `<Class>` is `Equipment`, its `<SlotType>` is some class's weapon slot, and it has at least one `<Projectile>`. Admin-only items, test files (`testing/*.xml`, `equipTest.xml`) and enchantment procs (`equipSubs.xml`) are left out.

Many weapons have Shiny or Retro reskins with the same `<DisplayId>`. A reskin with the same stats as the base item is dropped; a different projectile `<ObjectId>`, or a different `color`, `objectId` or `type` in an on-shoot effect, counts as looks, not stats. One with different stats is kept, with its `id` as its name so names stay unique.

| Field | Client source |
|---|---|
| `type`, `id` | `type` and `id` attributes |
| `name` | `<DisplayId>`, or `id` if there isn't one |
| `slotType`, `tier` | `<SlotType>`, `<Tier>` (no tier for untiered items) |
| `texture` | `{"file", "index"}` from `<Texture>` |
| `statBonuses` | `{"stat", "amount"}` from each `<ActivateOnEquip>IncrementStat</ActivateOnEquip>` |
| `projectiles` | one per `<Projectile>` |
| `subattacks` | one per `<Subattack>` |
| `onShoot` | one `{"type", "params"}` per `<OnPlayerShootActivate>`, read like an ability's `activates` (below) |
| `createdProjectiles` | as for abilities (below): the projectiles on-shoot effects name by `type` |
| `gear` | `{"itemType", "name", "slotType"}` for each item an on-shoot effect's `mustWear` names; `mustWear` items must all be worn |
| `labels` | `<Labels>`, split on commas. Enchantments match against them |
| `unsupported` | names of tags on the item that can change its damage but aren't modelled yet, e.g. `OnEnemyHitActivate` or `TurnRate` |

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
| `circleTurnDelay`, `circleTurnAngle` | `<CircleTurnDelay>` in ms and `<CircleTurnAngle>` in degrees. After the delay the shot turns by the angle and keeps that angle to the line from its spawn point: a circle at 90, an outward spiral below |
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

### Abilities

An item is an ability when its `<Class>` is `Equipment`, its `<SlotType>` is some class's ability slot, and it has `<Usable/>`. Items without it sit in ability slots only to hold a proc's projectiles. Admin-only items, test files and enchantment procs are left out, and reskins are merged, as with weapons.

| Field | Client source |
|---|---|
| `type`, `id`, `name`, `slotType`, `tier`, `texture`, `statBonuses` | as for weapons |
| `mpCost`, `cooldown` | `<MpCost>`, `<Cooldown>` in seconds, when set |
| `projectiles`, `subattacks` | as for weapons, when the ability fires any |
| `activates` | one `{"type", "params"}` per `<Activate>`: its text, like `BulletNova`, and its attributes |
| `createdProjectiles` | `{"type", "projectile"}` for each item an activate names in its `type` attribute (e.g. `type="0xD866"` on Midnight Star's afterimages): that item's `type` as a number and its first `<Projectile>`. Missing when no such item has one |
| `unsupported` | names of tags on the item that change how it's used but aren't modelled, e.g. `MpCostPerSecond` |

`params` keeps the attributes as the client has them: numbers where they parse as numbers, `true` and `false` as booleans, everything else, including hex colours, as strings. The engine reads the ones it understands and lists every effect type it doesn't model.

### Armor and rings

An item is an armor or a ring when its `<Class>` is `Equipment` and its `<SlotType>` is some class's armor or ring slot. Admin-only items and test files are left out, and reskins are merged, as with weapons.

| Field | Client source |
|---|---|
| `type`, `id`, `name`, `slotType`, `tier`, `texture`, `statBonuses` | as for weapons |
| `labels` | `<Labels>`, split on commas; enchantments match against them |
| `unsupported` | tags on the item that can change damage but aren't modelled, e.g. `OnPlayerHitActivate` |

### Targets

A short list of current endgame bosses, chosen by hand by object `id`. Most bosses also have old, legacy or event copies under other ids; the list names the ones in today's dungeons. The site uses them to fill in the target's DEF.

| Field | Client source |
|---|---|
| `id` | the enemy's `id` attribute |
| `name` | `<DisplayId>`, or a clearer name where several bosses share one (Oryx the Mad God 1, 2 and 3) |
| `defense` | `<Defense>`, 0 if missing |
| `maxHitPoints` | `<MaxHitPoints>` |

### Sets

Each `<EquipmentSet>` lists its pieces with `<Setpiece slot itemtype>`, where slot 0 is the weapon, 1 the ability, 2 the armor and 3 the ring. A slot can accept several items: a few "sets" are really one ring that goes with any of many weapons. Bonuses add up, so a full set gets the 2-piece, 3-piece and full-set stats together.

| Field | Client source |
|---|---|
| `type`, `name` | `type` and `id` attributes |
| `pieces` | `{"slot", "itemType", "name"}` per `<Setpiece>`; `name` is that item's `<DisplayId>` or `id`, empty if the client has no such item |
| `bonuses` | `{"pieces", "stats"}`: `stats` are `{"stat", "amount"}` from `<ActivateOnEquip2>`, `<ActivateOnEquip3>` and `<ActivateOnEquipAll>` `IncrementStat` elements, at 2, 3 and the set's number of slots |
| `unsupported` | other set effects, which aren't modelled. `ChangeSkin` only changes looks and is left out |

### Enchantments

An enchantment fits an item that has every label in `compatible` and none in `incompatible`, and isn't named in `incompatibleItems`. An armor or ring is matched on its own `labels`. An ability, which has none in the dataset, takes an enchantment when `compatible` only asks for `EQUIPMENT` and `ABILITY`, and `incompatible` rules out neither. Two enchantments can't share an item when one's `incompatibleLabels` includes one of the other's `labels`. An item holds up to 4.

| Field | Client source |
|---|---|
| `type`, `id`, `name`, `description` | `type` and `id` attributes, `<DisplayId>`, `<Description>` |
| `compatible`, `incompatible`, `incompatibleItems` | `<CompatibleWithItemLabels>`, `<IncompatibleWithItemLabels>`, `<IncompatibleWithItemIds>`, split on commas |
| `labels`, `incompatibleLabels` | `<EnchantmentLabels>`, `<IncompatibleWithEnchantmentLabels>` |
| `mutators` | `{"tag", "value", "params"}` per element under `<Mutators>`: its name, its text (a number like `1.05` or a kind like `IncrementStat`) and its attributes, read like an activate's `params` |
| `containers` | `{"id", "projectiles", "subattacks", "unsupported"}` for each item a `<SetSubAttack>` or `<AddSubAttack>` names in `subAttackObject`, read like a weapon. Its subattack `subAttackIndex` goes into the weapon's subattack slot given by the element's text |
| `texture` | `{"file", "index"}` from `<Texture>`, the enchantment's icon |

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

## Sprites

Sprites are optional: without them the simulation draws plain shapes. A release can include `sprites.png`, one image holding every sprite the site draws, and `sprites.json`, which says where each one is. Sprites come from the client's `characters` and `mapObjects` texture atlases, located through the `spritesheetf` TextAsset.

Every rectangle is `[x, y, width, height]` in pixels in `sprites.png`.

| Field | Meaning | Client source |
|---|---|---|
| `schemaVersion` | Format version, currently 1. Separate from the dataset's | |
| `build` | Client build, as in the dataset | |
| `projectiles` | By projectile `objectId`: `{"rect", "angleCorrection", "rotation"}` | the projectile object's `<Texture>` (or the first of its `<RandomTexture>`), `<AngleCorrection>` and `<Rotation>` |
| `weapons` | By weapon `type`, as a string: the item's icon | the weapon's `<Texture>` |
| `items` | By ability, armor or ring `type`, as a string: the item's icon | the item's `<Texture>` |
| `enchantments` | By enchantment `id`: its icon | the enchantment's `<Texture>` |
| `classes` | By class `id`: `{"stand", "attack": [...]}`, seen from behind | the class's `<AnimatedTexture>`, facing up, standing and attacking |
| `target` | The target the simulation shoots at | the `Target Strong` object's `<Texture>` |

Projectile sprites point right. When a shot is drawn along its heading, `angleCorrection` turns it by that many 45 degree steps, and `rotation` spins it in degrees per second.

## Deploying

Pushing to `main`, or publishing a new `data-*` release, runs `.github/workflows/pages.yml`. It builds the engine and the site, downloads the newest dataset, checks that every weapon and ability in it evaluates, that the hit estimate matches what the wiki says about tiered bows, longbows and the Torrential Staff, that the simulation lands exactly the estimated hits and can cast every ability, then deploys to GitHub Pages.

To roll the data back, run the workflow by hand with an older release:

```
gh workflow run pages.yml -f data=data-6eae0101
```

The site keeps that dataset until the next push or data release.

A release can also include `status.json`, which the site uses to show how old its data is:

| Field | Meaning |
|---|---|
| `schemaVersion` | Format version, currently 1 |
| `build` | Client build, as in the dataset |
| `extractedAt` | When the data was read from the client, UTC |
| `pipeline` | Commit of the tool that made the dataset |
| `classes`, `weapons`, `abilities` | How many of each the dataset has |
| `coverage` | `weaponsWithGaps`: weapons with something the engine ignores; `unknownTags`: client tags the tool hasn't seen before |

## License

AGPL-3.0. See [LICENSE](LICENSE).
