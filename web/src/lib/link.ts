// The build in the page's URL, so a link opens the same build. Anything
// missing or no longer in the dataset falls back to the defaults.

import { abilitiesFor, weaponsFor, type Dataset } from "./dataset";
import type { PlayerStatus, Target } from "./engine";

export interface Build {
  classId: string;
  weaponName: string;
  abilityName: string;
  attack: number;
  dexterity: number;
  wisdom: number;
  status: PlayerStatus;
  target: Target;
  distance: number;
  radius: number;
  wearing: number[];
}

const statusKeys = ["weak", "damaging", "berserk", "dazed"] as const;
const targetKeys = ["armored", "armorBroken", "exposed", "cursed"] as const;

export function toQuery(b: Build): string {
  const q = new URLSearchParams({
    class: b.classId,
    weapon: b.weaponName,
    att: String(b.attack),
    dex: String(b.dexterity),
    wis: String(b.wisdom),
    def: String(b.target.defense),
    dist: String(b.distance),
    size: String(b.radius),
  });
  if (b.abilityName) {
    q.set("ability", b.abilityName);
  }
  const self = statusKeys.filter((k) => b.status[k]);
  if (self.length) {
    q.set("self", self.join(","));
  }
  const on = targetKeys.filter((k) => b.target[k]);
  if (on.length) {
    q.set("target", on.join(","));
  }
  if (b.wearing.length) {
    q.set("wear", b.wearing.join(","));
  }
  return q.toString();
}

/**
 * Null when the query doesn't name a class and weapon this dataset has.
 * Stats it leaves out are the class's max.
 */
export function fromQuery(query: string, dataset: Dataset): Build | null {
  const q = new URLSearchParams(query);
  const cls = dataset.classes.find((c) => c.id === q.get("class"));
  const weapon = cls && weaponsFor(dataset, cls).find((w) => w.name === q.get("weapon"));
  if (!cls || !weapon) {
    return null;
  }
  const ability = abilitiesFor(dataset, cls).find((a) => a.name === q.get("ability"));
  const number = (key: string, otherwise: number) => {
    const n = Number(q.get(key));
    return q.has(key) && Number.isFinite(n) && n >= 0 ? n : otherwise;
  };
  const flags = (key: string) => new Set((q.get(key) ?? "").split(","));
  const self = flags("self");
  const on = flags("target");
  return {
    classId: cls.id,
    weaponName: weapon.name,
    abilityName: ability?.name ?? "",
    attack: number("att", cls.attack.max),
    dexterity: number("dex", cls.dexterity.max),
    wisdom: number("wis", cls.wisdom?.max ?? 0),
    status: Object.fromEntries(statusKeys.map((k) => [k, self.has(k)])) as unknown as PlayerStatus,
    target: {
      defense: number("def", 0),
      ...(Object.fromEntries(targetKeys.map((k) => [k, on.has(k)])) as Omit<Target, "defense">),
    },
    distance: number("dist", 4),
    radius: number("size", 0.5),
    wearing: [...flags("wear")].map(Number).filter((n) => Number.isInteger(n) && n > 0),
  };
}
