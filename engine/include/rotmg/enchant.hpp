#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "rotmg/evaluate.hpp"
#include "rotmg/model.hpp"
#include "rotmg/stats.hpp"

namespace rotmg {

/// Whether the enchantment can go on this weapon.
bool Fits(const Enchantment& enchantment, const Weapon& weapon);

/// Whether it can go on any item of a slot, by the slot's label (RING,
/// ARMOR or ABILITY): it asks for no label beyond EQUIPMENT and that one.
bool FitsSlot(const Enchantment& enchantment, const std::string& slot_label);

/// Whether it can go on this armor or ring, by the item's own labels.
bool Fits(const Enchantment& enchantment, const Gear& gear);

/// Whether two enchantments can share an item.
bool Conflict(const Enchantment& a, const Enchantment& b);

/// Legacy enchantments stay on the items that have them, but can't be
/// obtained any more.
bool Legacy(const Enchantment& enchantment);

/// How an enchantment can come to be on an item.
enum class Obtained {
  /// It doesn't fit, the item takes no enchantments, or it's a legacy one.
  kNever,
  /// It's in the pool the item rolls from.
  kRolls,
  /// It fits but isn't in the pool, so only an artifact or an engraving
  /// puts it there, like an Alien enchantment on regular gear (RealmEye:
  /// Artifacts).
  kArtifact,
};

/// In datasets from before pools, everything that fits rolls.
Obtained HowObtained(const Enchantment& enchantment, const Weapon& item, const Dataset& dataset);
Obtained HowObtained(const Enchantment& enchantment, const Ability& item, const Dataset& dataset);
Obtained HowObtained(const Enchantment& enchantment, const Gear& item, const Dataset& dataset);

/// The items of a slot (0 weapon, 1 ability, 2 armor, 3 ring) marked
/// `variant_of` another, by name, each with the enchantments they can take
/// and it can't, if any. Wearing one without any of them is wearing the item
/// it plays like, so a search that also tries that item needn't try it.
/// `artifacts` says whether enchantments only an artifact or an engraving
/// puts on an item count.
std::map<std::string, std::vector<std::string>, std::less<>> VariantEnchantments(const Dataset& dataset, int slot,
                                                                                bool artifacts);

/// For any item of a slot, by the slot's label, as FitsSlot.
Obtained HowObtainedOnSlot(const Enchantment& enchantment, const std::string& slot_label);

/// How many enchantments an item holds: its slots, or 4 in datasets from
/// before slots.
int EnchantmentSlots(const Enchanting& enchanting, const Dataset& dataset);

/// A weapon with enchantments applied, the stats they add, and the ones
/// whose effects aren't modelled.
struct Enchanted {
  Weapon weapon;
  /// Every item's flat stats and conversions, for FinalStats.
  EnchantStats stats;
  /// MultiplyMPCost on the ability, all multiplied together.
  double mp_cost = 1;
  std::vector<std::string> unsupported;
};

/// Enchantments on the other gear, by slot label: RING, ARMOR or ABILITY.
using GearEnchantments = std::vector<std::pair<std::string, std::vector<const Enchantment*>>>;

/// Applies the weapon's enchantments: damage, fire rate, projectile speed
/// and lifetime multipliers, and awakened subattacks. Every item's flat and
/// relative stats, on-shoot effects (which join the weapon's) and the
/// ability's MP cost multipliers are collected too, the gear's in the order
/// given and then the weapon's.
Enchanted Enchant(const Weapon& weapon, const std::vector<const Enchantment*>& enchantments,
                  const GearEnchantments& gear = {});

}  // namespace rotmg
