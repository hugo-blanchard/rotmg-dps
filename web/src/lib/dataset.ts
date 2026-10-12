// The parts of the dataset the UI needs to build its menus. The engine reads
// the whole file itself; the format is documented in the README.

export interface StatRange {
  base: number;
  max: number;
}

export interface DatasetClass {
  type: number;
  id: string;
  weaponSlotType: number;
  attack: StatRange;
  dexterity: StatRange;
  // Missing from datasets made before abilities were added.
  abilitySlotType?: number;
  // Missing from datasets made before armor and rings were added.
  armorSlotType?: number;
  ringSlotType?: number;
  maxHitPoints?: StatRange;
  maxMagicPoints?: StatRange;
  defense?: StatRange;
  speed?: StatRange;
  vitality?: StatRange;
  wisdom?: StatRange;
}

// How an item takes enchantments, and the item it's a reskin of. Missing
// from older datasets.
interface ItemRules {
  enchantmentSlots?: number;
  enchantmentPool?: string;
  /** The item this one plays exactly like, when it's a reskin of it. */
  reskinOf?: string;
}

export interface DatasetAbility extends ItemRules {
  type: number;
  id: string;
  name: string;
  slotType: number;
  tier?: number;
  statBonuses?: { stat: string; amount: number }[];
  projectiles?: { id: number; objectId: string; size?: number }[];
  createdProjectiles?: { type: number; projectile: { objectId: string; size?: number } }[];
  // Missing from older datasets, where an ability's enchantments match on
  // the slot.
  labels?: string[];
}

export interface DatasetWeapon extends ItemRules {
  type: number;
  id: string;
  name: string;
  slotType: number;
  tier?: number;
  statBonuses?: { stat: string; amount: number }[];
  projectiles?: { id: number; objectId: string; size?: number }[];
  // Items the weapon's on-shoot effects need worn, like a ring.
  gear?: { itemType: number; name: string; slotType: number }[];
  // Item labels, which enchantments match against.
  labels?: string[];
  unsupported?: string[];
}

// An armor or a ring.
export interface DatasetGear extends ItemRules {
  type: number;
  id: string;
  name: string;
  slotType: number;
  tier?: number;
  statBonuses?: { stat: string; amount: number }[];
  labels?: string[];
  unsupported?: string[];
}

export interface DatasetEnchantment {
  type: number;
  id: string;
  name: string;
  description: string;
  compatible: string[];
  incompatible?: string[];
  incompatibleItems?: string[];
  labels: string[];
  incompatibleLabels?: string[];
  texture?: { file: string; index: number };
}

// A boss to aim at, with its DEF from the client.
export interface DatasetTarget {
  id: string;
  name: string;
  defense: number;
  maxHitPoints: number;
}

// An equipment set. Slots are 0 weapon, 1 ability, 2 armor, 3 ring.
export interface DatasetSet {
  type: number;
  name: string;
  pieces: { slot: number; itemType: number; name: string }[];
  bonuses?: { pieces: number; stats: { stat: string; amount: number }[] }[];
  unsupported?: string[];
}

export interface Dataset {
  schemaVersion: number;
  build: string;
  classes: DatasetClass[];
  weapons: DatasetWeapon[];
  abilities?: DatasetAbility[];
  targets?: DatasetTarget[];
  sets?: DatasetSet[];
  enchantments?: DatasetEnchantment[];
  armors?: DatasetGear[];
  rings?: DatasetGear[];
  // The pools items roll enchantments from. Missing from older datasets.
  enchantmentPools?: { name: string; enchantments: string[] }[];
}

// Written by the pipeline next to the dataset. Older releases don't have one.
export interface DatasetStatus {
  schemaVersion: number;
  build: string;
  extractedAt: string;
}

/** Null when the release has no status.json. */
export async function loadStatus(): Promise<DatasetStatus | null> {
  try {
    const response = await fetch(`${import.meta.env.BASE_URL}data/status.json`);
    if (!response.ok) {
      return null;
    }
    const status = (await response.json()) as DatasetStatus;
    return status.schemaVersion === 1 ? status : null;
  } catch {
    return null;
  }
}

/** Fetches the dataset deployed next to the site. `text` goes to the engine as is. */
export async function loadDataset(): Promise<{ text: string; dataset: Dataset }> {
  const response = await fetch(`${import.meta.env.BASE_URL}data/dataset.json`);
  if (!response.ok) {
    throw new Error(`couldn't load the dataset (HTTP ${response.status})`);
  }
  const text = await response.text();
  return { text, dataset: JSON.parse(text) as Dataset };
}

// Tiered items by tier, highest first, then the rest by name.
function byTierThenName(a: { tier?: number; name: string }, b: { tier?: number; name: string }): number {
  if (a.tier !== undefined || b.tier !== undefined) {
    return (b.tier ?? -1) - (a.tier ?? -1);
  }
  return a.name.localeCompare(b.name);
}

/** Weapons a class can use: tiered ones by tier, highest first, then the rest by name. */
export function weaponsFor(dataset: Dataset, cls: DatasetClass): DatasetWeapon[] {
  return dataset.weapons.filter((w) => w.slotType === cls.weaponSlotType).sort(byTierThenName);
}

/** Abilities a class can use, in the same order as weapons. */
export function abilitiesFor(dataset: Dataset, cls: DatasetClass): DatasetAbility[] {
  return (dataset.abilities ?? []).filter((a) => a.slotType === cls.abilitySlotType).sort(byTierThenName);
}

/** Armor a class can wear, in the same order as weapons. */
export function armorsFor(dataset: Dataset, cls: DatasetClass): DatasetGear[] {
  return (dataset.armors ?? []).filter((a) => a.slotType === cls.armorSlotType).sort(byTierThenName);
}

/** Rings a class can wear, in the same order as weapons. */
export function ringsFor(dataset: Dataset, cls: DatasetClass): DatasetGear[] {
  return (dataset.rings ?? []).filter((r) => r.slotType === cls.ringSlotType).sort(byTierThenName);
}

/** The four equipment slots, in the game's order. */
export const slots = ["weapon", "ability", "armor", "ring"] as const;
export type Slot = (typeof slots)[number];

/** An item in any slot: what the equipment panel and pickers need. */
export type Item = DatasetWeapon | DatasetAbility | DatasetGear;

/** The items a class can put in a slot. */
export function itemsFor(dataset: Dataset, cls: DatasetClass, slot: Slot): Item[] {
  switch (slot) {
    case "weapon":
      return weaponsFor(dataset, cls);
    case "ability":
      return abilitiesFor(dataset, cls);
    case "armor":
      return armorsFor(dataset, cls);
    case "ring":
      return ringsFor(dataset, cls);
  }
}

/** An enchantment an item can have, and whether it rolls from the item's
 * pool or only an artifact or engraving puts it there. */
export interface EnchantmentOption {
  enchantment: DatasetEnchantment;
  rolls: boolean;
}

const poolSets = new WeakMap<Dataset, Map<string, Set<string>>>();
function pools(dataset: Dataset): Map<string, Set<string>> {
  let out = poolSets.get(dataset);
  if (!out) {
    out = new Map((dataset.enchantmentPools ?? []).map((p) => [p.name, new Set(p.enchantments)]));
    poolSets.set(dataset, out);
  }
  return out;
}

/**
 * Enchantments that can go on an item, by the engine's rules: ones that fit
 * its labels, except legacy ones nobody can get any more. In datasets with
 * pools, an item without slots takes none, and ones outside its pool need
 * an artifact. Abilities without labels in older datasets, which have no
 * pools, take what fits the slot.
 */
export function enchantmentsForItem(dataset: Dataset, slot: Slot, item: Item | undefined): EnchantmentOption[] {
  if (!item) {
    return [];
  }
  const labels = item.labels ?? [];
  const byPool = pools(dataset).size > 0;
  const bySlot = slot === "ability" && labels.length === 0 && !byPool;
  if (byPool && !item.enchantmentSlots) {
    return [];
  }
  const pool = byPool ? pools(dataset).get(item.enchantmentPool ?? "") : undefined;
  const generic = (l: string) => l === "EQUIPMENT" || l === "ABILITY";
  return (dataset.enchantments ?? [])
    .filter(
      (e) =>
        e.compatible.length > 0 &&
        !e.labels.includes("LEGACY") &&
        (bySlot
          ? e.compatible.every(generic) && !(e.incompatible ?? []).some(generic)
          : e.compatible.every((l) => labels.includes(l)) &&
            !(e.incompatible ?? []).some((l) => labels.includes(l)) &&
            !(e.incompatibleItems ?? []).includes(item.id)),
    )
    .map((e) => ({ enchantment: e, rolls: !pool || pool.has(e.id) }));
}

/** How many enchantments an item holds: its slots, or 4 in datasets from
 * before slots. */
export function enchantmentSlotsOf(dataset: Dataset, item: Item | undefined): number {
  if (!item) {
    return 0;
  }
  return pools(dataset).size > 0 ? Math.min(item.enchantmentSlots ?? 0, 4) : 4;
}

/**
 * An item's enchantment slots with only what can be there together: ids
 * among `options`, in the item's first `count` slots, no repeats or clashes
 * with an earlier slot. Slots keep their place; an empty or dropped one is
 * "". Always 4 long.
 */
export function usableSlots(ids: string[], options: DatasetEnchantment[], count = 4): string[] {
  const kept: DatasetEnchantment[] = [];
  return [0, 1, 2, 3].map((i) => {
    const e = options.find((x) => x.id === ids[i]);
    if (i >= count || !e || kept.some((k) => k.id === e.id || clash(k, e))) {
      return "";
    }
    kept.push(e);
    return e.id;
  });
}

/** Whether two enchantments can't share an item. */
export function clash(a: DatasetEnchantment, b: DatasetEnchantment): boolean {
  const meets = (x: DatasetEnchantment, y: DatasetEnchantment) =>
    (x.incompatibleLabels ?? []).some((l) => y.labels.includes(l));
  return meets(a, b) || meets(b, a);
}

/** Class stats at their max: builds are always for a maxed character. */
export function maxStats(cls: DatasetClass) {
  return {
    attack: cls.attack.max,
    dexterity: cls.dexterity.max,
    wisdom: cls.wisdom?.max ?? 0,
    vitality: cls.vitality?.max ?? 0,
    defense: cls.defense?.max ?? 0,
    speed: cls.speed?.max ?? 0,
    maxHitPoints: cls.maxHitPoints?.max ?? 0,
    maxMagicPoints: cls.maxMagicPoints?.max ?? 0,
  };
}
