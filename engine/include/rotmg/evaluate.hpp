#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rotmg/damage.hpp"
#include "rotmg/hits.hpp"
#include "rotmg/model.hpp"

namespace rotmg {

/// Stats from everything except the item being evaluated. Its own stat
/// bonuses are added during evaluation, so swapping items compares fairly.
struct Character {
  int attack = 0;
  int dexterity = 0;
  int wisdom = 0;
  int vitality = 0;
  int defense = 0;
  int speed = 0;
  int max_hit_points = 0;
  int max_magic_points = 0;
  PlayerStatus status{};
  /// Items worn besides the one being evaluated, by type: the ability,
  /// armor and ring. Some on-shoot effects need them.
  std::vector<int> worn{};

  /// A stat by the client's name: ATT, DEX, WIS, VIT, DEF, SPD, MAXHP or
  /// MAXMP. 0 for anything else.
  int Stat(std::string_view client_name) const;

  /// This character with IncrementStat bonuses added.
  Character With(const std::vector<StatBonus>& bonuses) const;
};

/// Where each subattack's hits per volley come from, first match wins: the
/// user's value, a curated default, the geometric estimate at `distance`,
/// and finally every projectile hitting.
struct Assumptions {
  /// Set by the user, by subattack index.
  std::vector<std::optional<double>> hits_per_volley;
  /// Reviewed defaults from curated/hit-defaults.json, by subattack index.
  std::vector<std::optional<double>> curated_hits_per_volley;
  /// Distance to the target in tiles. Unset means no geometric estimate.
  std::optional<double> distance;
  double target_radius = kDefaultTargetRadius;
};

enum class Source {
  kWiki,               // a rule from the RealmEye wiki
  kClientData,         // a value read straight from the dataset
  kGeometricEstimate,  // the hit model in hits.hpp
  kCurated,            // a reviewed default with its own source
  kUser,               // set by the user
  kAssumed,            // no source yet; listed so the UI can say so
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
  int projectiles_per_volley = 0;
  double volleys_per_second = 0;
  double hits_per_volley = 0;
  Source hits_source = Source::kAssumed;
  double damage_per_hit = 0;
  double dps = 0;
};

/// Damage from an on-shoot effect rather than the weapon's projectiles.
struct ProcResult {
  std::string name;
  double per_second = 0;
  double damage = 0;
  double dps = 0;
  std::string reference;
};

struct Breakdown {
  /// Projectiles plus procs.
  double dps = 0;
  std::vector<SubattackResult> subattacks;
  std::vector<ProcResult> procs;
  std::vector<Term> terms;
  /// Rules applied without a source, in plain words.
  std::vector<std::string> assumptions;
  /// Mechanics on this weapon the engine doesn't model, from the dataset.
  std::vector<std::string> unsupported;
};

/// Stat changes a weapon's on-shoot effects keep up while it fires, like
/// Valor's -10 ATT. Only the ones the engine can model.
std::vector<StatBonus> WhileShooting(const Weapon& weapon);

/// Single-target DPS of a weapon. Each subattack fires on its own at its own
/// rate (RealmEye: Weapon Attributes), so the total is their sum.
Breakdown EvaluateWeapon(const Weapon& weapon, const Character& character, const Target& target,
                         const Assumptions& assumptions = {});

}  // namespace rotmg
