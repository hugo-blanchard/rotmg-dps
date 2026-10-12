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

/// A target that can only be hurt in short windows, like a boss that's
/// vulnerable for a moment every few seconds.
struct BurstWindow {
  /// Seconds. 0 is a single moment: one volley and one cast.
  double length_s = 0;
  /// Seconds from one window's start to the next's.
  double every_s = 10;
};

struct BurstBreakdown {
  /// In one window.
  double damage = 0;
  /// The weapon's projectiles and on-shoot effects.
  double weapon_damage = 0;
  double ability_damage = 0;
  /// Casts in a window, on average over many: a share of one when MP runs
  /// short.
  double casts = 0;
  std::vector<std::string> assumptions;
};

/// Damage in each window from a player who times everything to it: they
/// fire from the moment it opens and cast the ability then, and again
/// whenever its cooldown and MP allow, and MP regenerates in between, up to
/// the max. Only what's fired or cast in the window counts, and of damage
/// dealt over time only what lands in it.
BurstBreakdown EvaluateBurst(const Weapon& weapon, const Ability* ability, const Character& character,
                             const Target& target, const Assumptions& assumptions, const BurstWindow& window);

}  // namespace rotmg
