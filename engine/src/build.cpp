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
      target.damage_taken = std::max(target.damage_taken, b->factor);
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
  return EvaluateWeaponDps(weapon, character, target, assumptions);
}

// The weapon's DPS averaged over its chance boosts being up or not, each on
// its own, with whatever else is up.
double MixedDps(const Weapon& weapon, const Character& character, const Target& target,
                const Assumptions& assumptions, const std::vector<const Buff*>& active,
                const std::vector<ChanceBoost>& boosts) {
  double total = 0;
  for (unsigned mask = 0; mask < (1u << boosts.size()); ++mask) {
    double weight = 1;
    Character c = character;
    for (std::size_t i = 0; i < boosts.size(); ++i) {
      const bool up = (mask >> i) & 1u;
      weight *= up ? boosts[i].share : 1 - boosts[i].share;
      if (up) {
        c = c.With({boosts[i].bonus});
      }
    }
    if (weight > 0) {
      total += weight * WeaponDps(weapon, c, target, assumptions, active);
    }
  }
  return total;
}

}  // namespace

BuildBreakdown EvaluateBuild(const Weapon& weapon, const Ability* ability, const Character& character,
                             const Target& target, const Assumptions& assumptions) {
  BuildBreakdown out;
  const Character with_ability_gear = ability ? character.With(ability->stat_bonuses) : character;
  out.weapon = EvaluateWeapon(weapon, with_ability_gear, target, assumptions);
  double shots_per_second = 0;
  for (const SubattackResult& s : out.weapon.subattacks) {
    shots_per_second = std::max(shots_per_second, s.volleys_per_second);
  }
  const std::vector<ChanceBoost> boosts = ChanceBoosts(weapon, character.worn, shots_per_second);
  for (const ChanceBoost& b : boosts) {
    out.uptimes.push_back({{Buff::On::kPlayer, b.bonus.stat, b.bonus.amount, b.duration_s}, b.share});
  }
  if (!boosts.empty()) {
    out.assumptions.push_back(
        "an on-shoot boost's cooldown starts when it goes off, and boosts go off independently of each other, with "
        "ones of the same stat counting once");
  }
  out.weapon_dps =
      boosts.empty() ? out.weapon.dps : MixedDps(weapon, with_ability_gear, target, assumptions, {}, boosts);
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
  const double no_buffs = out.weapon_dps;

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
    weighted += (end - start) * MixedDps(weapon, with_ability_gear, target, assumptions, active, boosts);
    start = end;
  }
  if (start < period) {
    weighted += (period - start) * no_buffs;
  }
  out.weapon_dps = weighted / period;
  out.dps = out.weapon_dps + a.dps;
  out.assumptions.push_back(
      "every cast's buffs and debuffs start together and run their full duration, and a new cast doesn't extend "
      "one that's still up");
  return out;
}

}  // namespace rotmg
