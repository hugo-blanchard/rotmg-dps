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

export interface Dataset {
  schemaVersion: number;
  build: string;
  classes: DatasetClass[];
  weapons: DatasetWeapon[];
  abilities?: DatasetAbility[];
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
