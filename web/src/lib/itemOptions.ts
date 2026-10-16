// Items as picker options, for the equipment panel and the optimizer.

import type { PickerOption } from "./Picker.svelte";
import { itemEffects, statSummary, type Dataset, type Item } from "./dataset";
import { itemIcon, type Sprites } from "./sprites";

/** The picker's buttons for narrowing a list of items. */
export const itemFilters = [
  { key: "tiered", label: "Tiered" },
  { key: "untiered", label: "Untiered" },
  { key: "set", label: "Set pieces" },
  { key: "variant", label: "Special reskins" },
  { key: "unobtainable", label: "Unobtainable" },
  { key: "reskin", label: "Reskins" },
];

/** An item's tier and stats, like "T12 · +7 ATT". */
export function describeItem(item: Item): string {
  return [item.tier !== undefined ? `T${item.tier}` : "", statSummary(item)].filter(Boolean).join(" · ");
}

/**
 * The items as picker options. Reskins that take their own enchantments,
 * items no one can get, then reskins, which play exactly like another item,
 * go last under their own headings.
 */
export function itemOptions(dataset: Dataset, items: Item[], sprites: Sprites | null): PickerOption[] {
  const setTypes = new Set((dataset.sets ?? []).flatMap((s) => s.pieces.map((p) => p.itemType)));
  const kind = (i: Item) => (i.unobtainable ? "unobtainable" : i.reskinOf ? "reskin" : i.variantOf ? "variant" : "");
  const detail = (i: Item) =>
    kind(i) === "reskin"
      ? `Reskin of ${i.reskinOf}`
      : kind(i) === "variant"
        ? `${i.variantOf} with its own enchantments`
        : describeItem(i);
  const option = (i: Item): PickerOption => ({
    key: i.name,
    name: i.name,
    icon: itemIcon(sprites, i.type),
    detail: detail(i),
    effects: itemEffects(i),
    group: {
      variant: "Reskins with special enchantments",
      unobtainable: "Unobtainable",
      reskin: "Reskins",
      "": undefined,
    }[kind(i)],
    tags: [
      i.tier !== undefined ? "tiered" : "untiered",
      ...(setTypes.has(i.type) ? ["set"] : []),
      ...(kind(i) ? [kind(i)] : []),
    ],
  });
  const of = (k: string) => items.filter((i) => kind(i) === k).map(option);
  return [...of(""), ...of("variant"), ...of("unobtainable"), ...of("reskin")];
}
