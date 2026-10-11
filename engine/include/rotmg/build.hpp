#pragma once

#include <optional>
#include <string>
#include <vector>

#include "rotmg/ability.hpp"
#include "rotmg/evaluate.hpp"

namespace rotmg {

/// How much of the time a buff or debuff is up.
struct Uptime {
  Buff buff;
  /// From 0 to 1.
  double share = 0;
};

struct BuildBreakdown {
  /// Weapon, with the ability's buffs and debuffs, plus the ability's own damage.
  double dps = 0;
  /// The weapon's DPS averaged over a cast cycle of the ability.
  double weapon_dps = 0;
  /// The weapon on its own, with the ability's stat bonuses but none of its effects.
  Breakdown weapon;
  std::optional<AbilityBreakdown> ability;
  std::vector<Uptime> uptimes;
  std::vector<std::string> assumptions;
};

/// A weapon and, optionally, an ability. Each cast's buffs and debuffs start
/// together and run for their duration, capped at the time until the next
/// cast. The cycle is split where they end, and the weapon's DPS is worked
/// out for each piece with whatever is up.
BuildBreakdown EvaluateBuild(const Weapon& weapon, const Ability* ability, const Character& character,
                             const Target& target, const Assumptions& assumptions);

}  // namespace rotmg
