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

/// How much the weapon's and the ability's DPS count.
struct DpsWeights {
  double weapon = 1;
  double ability = 1;
};

/// EvaluateBuild's three numbers without its explanation, for searches that
/// score many builds. With equal weights they're EvaluateBuild's to the last
/// bit. Otherwise the ability is held back when that does more for the
/// weighted DPS rather than the total, as a player after ability damage
/// would time their casts; that keeps the weighted DPS from ever falling
/// when a stat rises.
struct BuildNumbers {
  double dps = 0;
  double weapon_dps = 0;
  double ability_dps = 0;
};
BuildNumbers EvaluateBuildNumbers(const Weapon& weapon, const Ability* ability, const Character& character,
                                  const Target& target, const Assumptions& assumptions,
                                  const DpsWeights& weights = {});

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
  /// What limits them: how many fit in the window from a full pool of MP,
  /// as the cooldown allows, and how many the MP regenerated between windows
  /// pays for, -1 for an ability that costs none.
  int window_casts = 0;
  double paid_casts = -1;
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
