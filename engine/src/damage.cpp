#include "rotmg/damage.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <cmath>

namespace rotmg {

Multiplier DamageMultiplier(int attack, const PlayerStatus& status) {
  if (status.weak) {
    return {1, 2};
  }
  if (status.damaging) {
    return {(attack + 25) * 5, 50 * 4};
  }
  return {attack + 25, 50};
}

int HitDamage(int roll, Multiplier multiplier) {
  return roll * multiplier.num / multiplier.den;
}

int EffectiveDefense(const Target& target, bool armor_piercing) {
  int defense = 0;
  if (!armor_piercing) {
    defense = target.defense;
    // Armored before Exposed is our ordering; the wiki doesn't say.
    if (target.armored) {
      defense = defense * 3 / 2;
    }
    if (target.armor_broken && defense > 0) {
      defense = 0;
    }
  }
  if (target.exposed) {
    defense -= 20;
  }
  return defense;
}

double DamageAfterDefense(double damage, int defense, bool cursed) {
  double after = std::max(damage - defense, 0.1 * damage);
  return cursed ? after * 1.25 : after;
}

Multiplier WithDamageBonus(Multiplier multiplier, double bonus) {
  if (bonus == 1) {
    return multiplier;
  }
  return {multiplier.num * static_cast<int>(std::lround(bonus * 10000)), multiplier.den * 10000};
}

double ExpectedHitDamage(const Projectile& projectile, Multiplier multiplier, const Target& target) {
  const int defense = EffectiveDefense(target, projectile.armor_piercing);
  const int low = projectile.min_damage;
  // When min == max (placeholder projectiles are 0-0) there is a single roll.
  const int high = std::max(low, projectile.max_damage - 1);
  // Averaging every roll is slow for wide ranges, and a build's DPS asks
  // for the same averages once per buff window, so they're remembered. The
  // key is everything the average depends on.
  static std::map<std::tuple<int, int, int, int, int, bool>, double> known;
  if (known.size() > 65536) {
    known.clear();
  }
  const auto key = std::tuple{low, high, multiplier.num, multiplier.den, defense, target.cursed};
  auto it = known.find(key);
  if (it == known.end()) {
    double total = 0;
    for (int roll = low; roll <= high; ++roll) {
      total += DamageAfterDefense(HitDamage(roll, multiplier), defense, target.cursed);
    }
    it = known.emplace(key, total / (high - low + 1)).first;
  }
  return it->second * target.damage_taken;
}

double ExpectedAbilityDamage(int low, int high, double bonus, const Target& target, bool armor_piercing) {
  const int defense = EffectiveDefense(target, armor_piercing);
  high = std::max(low, high);
  double total = 0;
  for (int roll = low; roll <= high; ++roll) {
    total += DamageAfterDefense(roll + bonus, defense, target.cursed);
  }
  return total / (high - low + 1) * target.damage_taken;
}

}  // namespace rotmg
