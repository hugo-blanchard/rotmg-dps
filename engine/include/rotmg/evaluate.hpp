#pragma once

#include <optional>
#include <string>
#include <vector>

#include "rotmg/damage.hpp"
#include "rotmg/model.hpp"

namespace rotmg {

/// ATT and DEX from everything except the weapon. The weapon's own stat
/// bonuses are added during evaluation, so swapping weapons compares fairly.
struct Character {
  int attack = 0;
  int dexterity = 0;
  PlayerStatus status{};
};

struct Assumptions {
  /// Hits per volley for each subattack, by index. A missing entry means
  /// every projectile in the volley hits.
  std::vector<std::optional<double>> hits_per_volley;
};

enum class Source {
  kWiki,        // a rule from the RealmEye wiki
  kClientData,  // a value read straight from the dataset
  kUser,        // set by the user
  kAssumed,     // no source yet; listed so the UI can say so
};

/// One number in the breakdown, with where it comes from.
struct Term {
  std::string name;
  double value = 0;
  Source source = Source::kAssumed;
  std::string reference;
};

struct SubattackResult {
  int projectile_id = 0;
  double volleys_per_second = 0;
  double hits_per_volley = 0;
  double damage_per_hit = 0;
  double dps = 0;
};

struct Breakdown {
  double dps = 0;
  std::vector<SubattackResult> subattacks;
  std::vector<Term> terms;
  /// Rules applied without a source, in plain words.
  std::vector<std::string> assumptions;
  /// Mechanics on this weapon the engine doesn't model, from the dataset.
  std::vector<std::string> unsupported;
};

/// Single-target DPS of a weapon. Each subattack fires on its own at its own
/// rate (RealmEye: Weapon Attributes), so the total is their sum.
Breakdown EvaluateWeapon(const Weapon& weapon, const Character& character, const Target& target,
                         const Assumptions& assumptions = {});

}  // namespace rotmg
