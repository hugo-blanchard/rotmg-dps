#pragma once

#include <cstdint>

#include "rotmg/model.hpp"

// Damage of a single hit. Rules come from the RealmEye wiki pages
// Character Stats and Status Effects unless a comment says otherwise.
namespace rotmg {

/// Status effects on the player that change weapon damage or fire rate.
struct PlayerStatus {
  bool weak = false;
  bool damaging = false;
  bool berserk = false;
  bool dazed = false;
};

struct Target {
  int defense = 0;
  bool armored = false;
  bool armor_broken = false;
  bool exposed = false;
  bool cursed = false;
  /// Multiplies the damage it takes after everything else, like a poison's
  /// damage boost.
  double damage_taken = 1;
};

/// An exact fraction, so per-hit damage can be rounded down without floating
/// point error. In doubles, a roll of 50 at 4 ATT is 28.999... instead of 29.
/// 64-bit: with a damage bonus and Damaging, a big roll times the numerator
/// passes 2^31.
struct Multiplier {
  std::int64_t num = 1;
  std::int64_t den = 1;

  double Value() const { return static_cast<double>(num) / static_cast<double>(den); }
};

/// (ATT + 25) / 50. Weak makes it 0.5 whatever the ATT. Damaging adds 25%
/// unless the player is also Weak.
Multiplier DamageMultiplier(int attack, const PlayerStatus& status);

/// The multiplier with an item's damage bonus on top, kept exact to four
/// decimals: Damage Bonus IV's 1.05 multiplies the roll along with ATT.
Multiplier WithDamageBonus(Multiplier multiplier, double bonus);

/// Damage of one roll before DEF, rounded down. The wiki's worked examples
/// (83 ATT Sword of Acclaim = 475-591) only come out right with this floor.
int HitDamage(int roll, Multiplier multiplier);

/// The DEF a hit is reduced by. Armored adds 50% (rounded down), Armor Broken
/// then zeroes any positive DEF, and Exposed then subtracts 20, which can go
/// below zero. Armor-piercing hits skip DEF but still get Exposed's 20.
int EffectiveDefense(const Target& target, bool armor_piercing);

/// Damage after DEF, which can't take a hit below 10% of its damage, then
/// Curse's 25%. Not rounded, see the assumptions in EvaluateWeapon.
double DamageAfterDefense(double damage, int defense, bool cursed);

/// Average damage of one hit over every roll from min to max - 1. The listed
/// max is never rolled (RealmEye: Weapon Attributes).
double ExpectedHitDamage(const Projectile& projectile, Multiplier multiplier, const Target& target);

/// Average damage of one ability hit: a roll from `low` to `high`, both
/// included, plus a flat StatMod bonus, through DEF unless it pierces armor,
/// then Curse. ATT doesn't apply to abilities (RealmEye: Character Stats).
double ExpectedAbilityDamage(int low, int high, double bonus, const Target& target, bool armor_piercing);

}  // namespace rotmg
