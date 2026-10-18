// The build in the page's URL, so a link opens the same build. Anything
// missing or no longer in the dataset falls back to the defaults.

import { itemsFor, slots, type Dataset, type Slot } from "./dataset";
import type { PlayerStatus, Target } from "./engine";

export interface Build {
  classId: string;
  /** Item names by slot, "" for an empty one. */
  items: Record<Slot, string>;
  /** Enchantment ids by slot, 4 each, "" for an empty one. */
  enchants: Record<Slot, string[]>;
  status: PlayerStatus;
  /** The pet's Magic Heal level, 0 for none. */
  pet: number;
  target: Target;
  distance: number;
  radius: number;
}

const statusKeys = ["weak", "damaging", "berserk", "dazed"] as const;
const targetKeys = ["armored", "armorBroken", "exposed", "cursed"] as const;
// The weapon's enchantments keep the name they had before other gear had any.
const enchantKeys: Record<Slot, string> = {
  weapon: "ench",
  ability: "ench_ability",
  armor: "ench_armor",
  ring: "ench_ring",
};

export function toQuery(b: Build): string {
  const q = new URLSearchParams({ class: b.classId, weapon: b.items.weapon });
  for (const slot of ["ability", "armor", "ring"] as const) {
    if (b.items[slot]) {
      q.set(slot, b.items[slot]);
    }
  }
  q.set("pet", String(b.pet));
  q.set("def", String(b.target.defense));
  q.set("dist", String(b.distance));
  q.set("size", String(b.radius));
  const self = statusKeys.filter((k) => b.status[k]);
  if (self.length) {
    q.set("self", self.join(","));
  }
  const on = targetKeys.filter((k) => b.target[k]);
  if (on.length) {
    q.set("target", on.join(","));
  }
  for (const slot of slots) {
    // Slots keep their place, so empty ones in the middle stay as blanks.
    const list = [...b.enchants[slot]];
    while (list.length && !list[list.length - 1]) {
      list.pop();
    }
    if (list.length) {
      q.set(enchantKeys[slot], list.join(","));
    }
  }
  return q.toString();
}

/**
 * Null when the query doesn't name a class and weapon this dataset has.
 * Items that aren't for the class are left empty.
 */
export function fromQuery(query: string, dataset: Dataset): Build | null {
  const q = new URLSearchParams(query);
  const cls = dataset.classes.find((c) => c.id === q.get("class"));
  if (!cls || !itemsFor(dataset, cls, "weapon").some((w) => w.name === q.get("weapon"))) {
    return null;
  }
  const item = (slot: Slot) => itemsFor(dataset, cls, slot).find((i) => i.name === q.get(slot))?.name ?? "";
  const number = (key: string, otherwise: number) => {
    const n = Number(q.get(key));
    return q.has(key) && Number.isFinite(n) && n >= 0 ? n : otherwise;
  };
  const flags = (key: string) => new Set((q.get(key) ?? "").split(","));
  const known = (key: string) => {
    const ids = (q.get(key) ?? "").split(",").slice(0, 4);
    return [0, 1, 2, 3].map((i) => (dataset.enchantments?.some((e) => e.id === ids[i]) ? ids[i] : ""));
  };
  const self = flags("self");
  const on = flags("target");
  return {
    classId: cls.id,
    items: { weapon: item("weapon"), ability: item("ability"), armor: item("armor"), ring: item("ring") },
    enchants: Object.fromEntries(slots.map((s) => [s, known(enchantKeys[s])])) as Record<Slot, string[]>,
    status: Object.fromEntries(statusKeys.map((k) => [k, self.has(k)])) as unknown as PlayerStatus,
    pet: Math.min(100, Math.round(number("pet", 100))),
    target: {
      defense: number("def", 40),
      ...(Object.fromEntries(targetKeys.map((k) => [k, on.has(k)])) as Omit<Target, "defense">),
    },
    distance: number("dist", 4),
    radius: number("size", 0.5),
  };
}
