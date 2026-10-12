#pragma once

#include <string>
#include <vector>

#include "rotmg/evaluate.hpp"
#include "rotmg/model.hpp"

namespace rotmg {

/// Whether the enchantment can go on this weapon.
bool Fits(const Enchantment& enchantment, const Weapon& weapon);

/// Whether it can go on any item of a slot, by the slot's label (RING,
/// ARMOR or ABILITY): it asks for no label beyond EQUIPMENT and that one.
bool FitsSlot(const Enchantment& enchantment, const std::string& slot_label);

/// Whether two enchantments can share an item.
bool Conflict(const Enchantment& a, const Enchantment& b);

/// A weapon with enchantments applied, the stats they add, and the ones
/// whose effects aren't modelled.
struct Enchanted {
  /// "+16% ATT of Bonus ATT": `percent` of the bonus in `from` goes to `stat`.
  struct Relative {
    std::string stat;
    std::string from;
    double percent = 0;
  };
  Weapon weapon;
  std::vector<StatBonus> stats;
  std::vector<Relative> relative;
  /// MultiplyMPCost on the ability, all multiplied together.
  double mp_cost = 1;
  std::vector<std::string> unsupported;
};

/// Enchantments on the other gear, by slot label: RING, ARMOR or ABILITY.
using GearEnchantments = std::vector<std::pair<std::string, std::vector<const Enchantment*>>>;

/// Applies the weapon's enchantments: damage, fire rate, projectile speed
/// and lifetime multipliers, and awakened subattacks. Every item's flat and
/// relative stats, on-shoot effects (which join the weapon's) and the
/// ability's MP cost multipliers are collected too. Fractional stats (+1.8
/// VIT) add up over all items and the total rounds down; that's an
/// assumption.
Enchanted Enchant(const Weapon& weapon, const std::vector<const Enchantment*>& enchantments,
                  const GearEnchantments& gear = {});

/// What relative enchantments add, all in one final step so they don't
/// feed each other (RealmEye: Item Stat-Scaling). Bonus stats are "gear,
/// flat enchantments and exalts"; here that's everything in `character`
/// above the class's max. Each stat's total rounds down.
std::vector<StatBonus> RelativeBonuses(const std::vector<Enchanted::Relative>& relative, const Class& cls,
                                       const Character& character);

}  // namespace rotmg
