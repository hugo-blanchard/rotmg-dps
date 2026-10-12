#pragma once

#include <string>
#include <vector>

#include "rotmg/model.hpp"

namespace rotmg {

/// Whether the enchantment can go on this weapon.
bool Fits(const Enchantment& enchantment, const Weapon& weapon);

/// Whether two enchantments can share an item.
bool Conflict(const Enchantment& a, const Enchantment& b);

/// A weapon with enchantments applied, the stats they add, and the ones
/// whose effects aren't modelled.
struct Enchanted {
  Weapon weapon;
  std::vector<StatBonus> stats;
  std::vector<std::string> unsupported;
};

/// Applies damage, fire rate, projectile speed and lifetime multipliers,
/// and adds flat stat bonuses. Fractional stats (+1.8 VIT) add up and the
/// total rounds down; that's an assumption.
Enchanted Enchant(const Weapon& weapon, const std::vector<const Enchantment*>& enchantments);

}  // namespace rotmg
