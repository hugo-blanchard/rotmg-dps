# rotmg-dps

A DPS calculator for Realm of the Mad God (Exalt). It works out how much damage a build does to a given target, using item data from the game client and formulas sourced from the RealmEye wiki. A small simulation shows the character firing at a dummy so the numbers are easy to check.

Work in progress. There's nothing to run yet.

## Notice

Unofficial fan tool, not affiliated with DECA. Game assets © DECA Live Operations GmbH.

## Building

The engine is C++20 and needs CMake 3.25 or newer. Tests use Catch2, which CMake downloads on the first configure.

```
cmake -S engine -B engine/build
cmake --build engine/build
ctest --test-dir engine/build
```

Pass `-DROTMG_SANITIZE=ON` to build with AddressSanitizer and UBSan (GCC or Clang).

The web app needs Node 24.

```
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

## License

AGPL-3.0. See [LICENSE](LICENSE).
