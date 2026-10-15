#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "rotmg/damage.hpp"
#include "rotmg/hits.hpp"
#include "rotmg/model.hpp"

namespace rotmg {

/// Item types, held inline: a build wears a handful of items, and a
/// character is copied for every mix of buffs an evaluation looks at.
class ItemTypes {
 public:
  static constexpr std::size_t kMost = 16;

  ItemTypes() = default;
  ItemTypes(std::initializer_list<int> types) {
    for (const int t : types) push_back(t);
  }
  explicit ItemTypes(std::span<const int> types) {
    for (const int t : types) push_back(t);
  }

  /// Throws std::length_error past kMost.
  void push_back(int type) {
    if (size_ == kMost) {
      throw std::length_error("more than " + std::to_string(kMost) + " items worn");
    }
    types_[size_++] = type;
  }
  const int* data() const { return types_.data(); }
  const int* begin() const { return types_.data(); }
  const int* end() const { return types_.data() + size_; }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  bool operator==(const ItemTypes& other) const { return std::ranges::equal(*this, other); }

 private:
  std::array<int, kMost> types_{};
  std::size_t size_ = 0;
};

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
  ItemTypes worn{};

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

/// Whether the gear an effect asks for is on: every mustWear item, one of
/// the mustWearOR ones if it lists any, none of the mustNotWear ones.
bool GearWorn(const Activate& effect, std::span<const int> worn);

/// A stat boost with a chance per shot and a cooldown, like the OnShoot
/// boost enchantments. Ones that need HP or a status aren't modelled.
bool IsChanceBoost(const Activate& effect);

/// Stat changes a weapon's on-shoot effects keep up while it fires, like
/// Valor's -10 ATT. Only the ones the engine can model.
std::vector<StatBonus> WhileShooting(const Weapon& weapon);

/// How the engine models one of a weapon's on-shoot effects: kNone for an
/// effect it doesn't model, or whose gear isn't in `worn` (which includes the
/// weapon).
enum class OnShootModel { kNone, kWhileShooting, kPoisonBurst, kExtraShots, kChanceBoost };
OnShootModel ModelOf(const Weapon& weapon, const Activate& effect, std::span<const int> worn);

/// An on-shoot stat boost that goes off by chance, like the OnShoot Attack
/// Boost enchantment's 3% for +5 ATT, and the share of the time it's up.
struct ChanceBoost {
  StatBonus bonus;
  double duration_s = 0;
  double share = 0;
};

/// The weapon's chance boosts at `shots_per_second`. After a boost goes off
/// its cooldown runs; then each shot has the chance, so the next one comes
/// 1 / (chance x shots per second) seconds later on average. A boost is up
/// for its duration out of that cycle. Only the ones CountsAsChanceBoost
/// allows are listed.
std::vector<ChanceBoost> ChanceBoosts(const Weapon& weapon, std::span<const int> worn, double shots_per_second);

/// Whether one of the weapon's chance boosts counts, with `worn` including
/// the weapon. Boosts that don't stack with others of their stat (channel
/// noStack) don't add up at all: of those, only one per stat counts, the
/// biggest, then the longest, then the likeliest, then the first.
bool CountsAsChanceBoost(const Weapon& weapon, const Activate& boost, std::span<const int> worn);

/// Single-target DPS of a weapon. Each subattack fires on its own at its own
/// rate (RealmEye: Weapon Attributes), so the total is their sum.
Breakdown EvaluateWeapon(const Weapon& weapon, const Character& character, const Target& target,
                         const Assumptions& assumptions = {});

/// EvaluateWeapon's numbers alone: the DPS, subattacks and procs, without
/// the terms, assumptions and names that explain them, which are most of
/// the work. A proc here is one row per effect.
Breakdown EvaluateWeaponNumbers(const Weapon& weapon, const Character& character, const Target& target,
                                const Assumptions& assumptions = {});

/// EvaluateWeapon's DPS alone.
double EvaluateWeaponDps(const Weapon& weapon, const Character& character, const Target& target,
                         const Assumptions& assumptions = {});

}  // namespace rotmg
