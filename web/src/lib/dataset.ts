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
  maxHitPoints?: StatRange;
  maxMagicPoints?: StatRange;
  defense?: StatRange;
  speed?: StatRange;
  vitality?: StatRange;
  wisdom?: StatRange;
}

export interface DatasetAbility {
  type: number;
  id: string;
  name: string;
  slotType: number;
  tier?: number;
  statBonuses?: { stat: string; amount: number }[];
}

export interface DatasetWeapon {
  type: number;
  id: string;
  name: string;
  slotType: number;
  tier?: number;
  statBonuses?: { stat: string; amount: number }[];
  projectiles?: { id: number; objectId: string; size?: number }[];
  unsupported?: string[];
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

/** Sets with a piece among these items, like the chosen weapon and ability. */
export function setsFor(dataset: Dataset, items: ({ type: number } | undefined)[]): DatasetSet[] {
  const types = items.filter((i) => i !== undefined).map((i) => i.type);
  return (dataset.sets ?? []).filter((s) => s.pieces.some((p) => types.includes(p.itemType)));
}

/** A set's armor and ring pieces by slot: what you can say you also wear. */
export function otherPieces(set: DatasetSet): { slot: number; itemTypes: number[]; names: string[] }[] {
  const slots = new Map<number, { slot: number; itemTypes: number[]; names: string[] }>();
  for (const p of set.pieces.filter((p) => p.slot >= 2)) {
    const entry = slots.get(p.slot) ?? { slot: p.slot, itemTypes: [], names: [] };
    entry.itemTypes.push(p.itemType);
    if (p.name) {
      entry.names.push(p.name);
    }
    slots.set(p.slot, entry);
  }
  return [...slots.values()].sort((a, b) => a.slot - b.slot);
}

/** Class stats at their max, for the ones the editor doesn't show. */
export function maxStats(cls: DatasetClass) {
  return {
    vitality: cls.vitality?.max ?? 0,
    defense: cls.defense?.max ?? 0,
    speed: cls.speed?.max ?? 0,
    maxHitPoints: cls.maxHitPoints?.max ?? 0,
    maxMagicPoints: cls.maxMagicPoints?.max ?? 0,
  };
}
