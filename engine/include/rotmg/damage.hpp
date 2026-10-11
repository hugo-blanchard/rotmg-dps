#pragma once

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
};

/// An exact fraction, so per-hit damage can be rounded down without floating
/// point error. In doubles, a roll of 50 at 4 ATT is 28.999... instead of 29.
struct Multiplier {
  int num = 1;
  int den = 1;

  double Value() const { return static_cast<double>(num) / den; }
};

/// (ATT + 25) / 50. Weak makes it 0.5 whatever the ATT. Damaging adds 25%
/// unless the player is also Weak.
Multiplier DamageMultiplier(int attack, const PlayerStatus& status);

/// Damage of one roll before DEF, rounded down. The wiki's worked examples
/// (83 ATT Sword of Acclaim = 475-591) only come out right with this floor.
int HitDamage(int roll, Multiplier multiplier);

/// The DEF a hit is reduced by. Armored adds 50% (rounded down), Armor Broken
/// then zeroes any positive DEF, and Exposed then subtracts 20, which can go
/// below zero. Armor-piercing hits skip DEF but still get Exposed's 20.
int EffectiveDefense(const Target& target, bool armor_piercing);

/// Damage after DEF, which can't take a hit below 10% of its damage, then
/// Curse's 25%. Not rounded, see the assumptions in EvaluateWeapon.
double DamageAfterDefense(int damage, int defense, bool cursed);

/// Average damage of one hit over every roll from min to max - 1. The listed
/// max is never rolled (RealmEye: Weapon Attributes).
double ExpectedHitDamage(const Projectile& projectile, Multiplier multiplier, const Target& target);

}  // namespace rotmg
