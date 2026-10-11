#include "rotmg/build.hpp"

#include <algorithm>

namespace rotmg {
namespace {

// The weapon's DPS with a set of buffs and debuffs up, on top of whatever
// the player chose.
double WeaponDps(const Weapon& weapon, Character character, Target target, const Assumptions& assumptions,
                 const std::vector<const Buff*>& active) {
  for (const Buff* b : active) {
    if (b->on == Buff::On::kTarget) {
      target.cursed |= b->effect == "Curse";
      target.exposed |= b->effect == "Exposed";
      target.armor_broken |= b->effect == "Armor Broken";
    } else if (b->effect == "ATT") {
      character.attack += b->amount;
    } else if (b->effect == "DEX") {
      character.dexterity += b->amount;
    } else {
      character.status.damaging |= b->effect == "Damaging";
      character.status.berserk |= b->effect == "Berserk";
      character.status.weak |= b->effect == "Weak";
      character.status.dazed |= b->effect == "Dazed";
    }
  }
  return EvaluateWeapon(weapon, character, target, assumptions).dps;
}

}  // namespace

BuildBreakdown EvaluateBuild(const Weapon& weapon, const Ability* ability, const Character& character,
                             const Target& target, const Assumptions& assumptions) {
  BuildBreakdown out;
  const Character with_ability_gear = ability ? character.With(ability->stat_bonuses) : character;
  out.weapon = EvaluateWeapon(weapon, with_ability_gear, target, assumptions);
  out.weapon_dps = out.weapon.dps;
  out.dps = out.weapon_dps;
  if (ability == nullptr) {
    return out;
  }

  // The ability sees the weapon's stat bonuses too, as in game, and its
  // on-shoot stat changes, since the weapon is firing.
  out.ability =
      EvaluateAbility(*ability, character.With(weapon.stat_bonuses).With(WhileShooting(weapon)), target, assumptions);
  const AbilityBreakdown& a = *out.ability;
  out.dps += a.dps;
  if (a.buffs.empty() || a.casts_per_second <= 0) {
    return out;
  }

  const double period = 1 / a.casts_per_second;
  std::vector<double> lasts;
  for (const Buff& b : a.buffs) {
    const double up = std::clamp(b.duration_s, 0.0, period);
    lasts.push_back(up);
    out.uptimes.push_back({b, up / period});
  }
  std::vector<double> ends = lasts;
  std::ranges::sort(ends);
  ends.erase(std::unique(ends.begin(), ends.end()), ends.end());

  double start = 0;
  double weighted = 0;
  for (double end : ends) {
    if (end <= start) {
      continue;
    }
    std::vector<const Buff*> active;
    for (std::size_t i = 0; i < a.buffs.size(); ++i) {
      if (lasts[i] >= end) {
        active.push_back(&a.buffs[i]);
      }
    }
    weighted += (end - start) * WeaponDps(weapon, with_ability_gear, target, assumptions, active);
    start = end;
  }
  if (start < period) {
    weighted += (period - start) * out.weapon.dps;
  }
  out.weapon_dps = weighted / period;
  out.dps = out.weapon_dps + a.dps;
  out.assumptions.push_back(
      "every cast's buffs and debuffs start together and run their full duration, and a new cast doesn't extend "
      "one that's still up");
  return out;
}

}  // namespace rotmg
