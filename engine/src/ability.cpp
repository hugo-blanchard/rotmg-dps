#include "rotmg/ability.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>

#include "rotmg/damage.hpp"
#include "rotmg/hits.hpp"

namespace rotmg {
namespace {

// Effects that can't change single-target DPS: healing, movement, stealth,
// pets and the like. DamageMultAura is the shields' "you receive 75% damage"
// (wiki, Golden Shield): every use in the client lowers damage taken.
const std::set<std::string, std::less<>> kNoDamageEffects = {
    "Heal",           "Teleport",
    "Decoy",          "Sneak",
    "BoostRange",     "Pet",
    "Create",         "ClearConditionEffectSelf",
    "SelfTransform",  "RemoveNegativeConditionsSelf",
    "StasisBlast",    "MarkAndTeleport",
    "DamageMultAura",
};

// Status effects that change weapon DPS, by the side they land on.
const std::set<std::string, std::less<>> kPlayerStatuses = {"Damaging", "Berserk", "Weak", "Dazed"};
const std::set<std::string, std::less<>> kTargetStatuses = {"Curse", "Exposed", "Armor Broken", "ArmorBroken"};

std::string NormalizeStatus(std::string effect) {
  return effect == "ArmorBroken" ? "Armor Broken" : effect;
}

double Param(const Activate& a, std::string_view key) { return a.Number(key).value_or(0); }

}  // namespace

double MpPerSecond(int wisdom) { return 0.5 + 0.12 * wisdom; }

namespace {

// Points of `stat` over `threshold`, or 0.
double Over(const Character& c, const std::string& stat, double threshold) {
  return stat.empty() ? 0 : std::max(0.0, c.Stat(stat) - threshold);
}

}  // namespace

double LightningDamage(const Activate& a, const Character& c) {
  return a.Number("totalDamage").value_or(0) +
         Over(c, "WIS", a.Number("wisMin").value_or(50)) * a.Number("wisDamageBase").value_or(0) / 10;
}

int LightningTargets(const Activate& a, const Character& c) {
  const double per = a.Number("wisPerTarget").value_or(10);
  const double over = Over(c, "WIS", a.Number("wisMin").value_or(50));
  return static_cast<int>(a.Number("maxTargets").value_or(1)) +
         (per > 0 ? static_cast<int>(std::floor(over / per + 1e-9)) : 0);
}

double ShockblastDamage(const Activate& a, const Character& c) {
  const double over = Over(c, a.Text("scalingStat"), a.Number("statModScalingMin").value_or(0));
  return a.Number("aoeDamage").value_or(0) + over * a.Number("statModDamage").value_or(0) / 10;
}

int ShockblastTargets(const Activate& a, const Character& c) {
  const double per = a.Number("statModPerTarget").value_or(0);
  const double over = Over(c, a.Text("scalingStat"), a.Number("statModScalingMin").value_or(0));
  return static_cast<int>(a.Number("aoeMaxTargets").value_or(1)) +
         (per > 0 ? static_cast<int>(std::floor(over / per + 1e-9)) : 0);
}

std::optional<Buff> BuffOf(const Activate& a, const Character& c) {
  if (a.type == "ConditionEffectSelf" || a.type == "ConditionEffectAura") {
    const std::string effect = a.Text("effect");
    if (kPlayerStatuses.contains(effect)) {
      return Buff{Buff::On::kPlayer, effect, 0, Param(a, "duration") + StatModBonus(a, "statModDuration", c)};
    }
  } else if (a.type == "StatBoostSelf" || a.type == "StatBoostAura") {
    const std::string stat = a.Text("stat");
    if (stat == "ATT" || stat == "DEX") {
      return Buff{Buff::On::kPlayer, stat, static_cast<int>(Param(a, "amount")), Param(a, "duration")};
    }
  } else if (a.type == "EffectBlast") {
    // Duration grows by a second for every wisPerDuration WIS over wisMin
    // (RealmEye: Orbs gives "10 WIS -> +1 second" for wisPerDuration 10).
    const std::string effect = a.Text("condEffect");
    if (kTargetStatuses.contains(effect)) {
      double duration = Param(a, "condDuration");
      const double per = Param(a, "wisPerDuration");
      const double over = c.wisdom - Param(a, "wisMin");
      if (per > 0 && over > 0) {
        duration += over / per;
      }
      return Buff{Buff::On::kTarget, NormalizeStatus(effect), 0, duration};
    }
  }
  return std::nullopt;
}

double StatModBonus(const Activate& effect, std::string_view per_point_key, const Character& character) {
  const std::string stat = effect.Text("scalingStat");
  const auto per_point = effect.Number(per_point_key);
  if (stat.empty() || !per_point) {
    return 0;
  }
  const double over = character.Stat(stat) - effect.Number("statModScalingMin").value_or(0);
  return over > 0 ? *per_point * over : 0;
}

int StatModCount(const Activate& effect, std::string_view per_point_key, const Character& character) {
  // A small epsilon: 0.0667 per point over 15 points is meant to be 1 shot,
  // not 0.9999... rounded down.
  return static_cast<int>(std::floor(StatModBonus(effect, per_point_key, character) + 1e-9));
}

AbilityBreakdown EvaluateAbility(const Ability& ability, const Character& character, const Target& target,
                                 const Assumptions& assumptions) {
  AbilityBreakdown out;
  out.unsupported = ability.unsupported;
  const Character c = character.With(ability.stat_bonuses);
  const Weapon shots = ability.AsWeapon();
  const auto unsupported = [&out](std::string what) {
    if (std::ranges::find(out.unsupported, what) == out.unsupported.end()) {
      out.unsupported.push_back(std::move(what));
    }
  };
  const auto assume = [&out](std::string what) {
    if (std::ranges::find(out.assumptions, what) == out.assumptions.end()) {
      out.assumptions.push_back(std::move(what));
    }
  };

  for (const Activate& a : ability.activates) {
    if (a.type == "Shoot") {
      // Fires the item's own projectiles at the target, like a weapon volley.
      const double bonus = StatModBonus(a, "statModDamage", c);
      const bool estimate = assumptions.distance.has_value() && HitModelGaps(shots).empty();
      double damage = 0;
      for (const Subattack& s : shots.subattacks) {
        const Projectile* p = shots.FindProjectile(s.projectile_id);
        if (p == nullptr) {
          continue;
        }
        const int hits = estimate ? HitsPerVolley(shots, s, *assumptions.distance, assumptions.target_radius)
                                  : s.num_projectiles;
        damage += hits * ExpectedAbilityDamage(p->min_damage, p->max_damage - 1, bonus, target, p->armor_piercing);
        if (hits > 0) {
          for (const ConditionEffect& e : p->condition_effects) {
            if (kTargetStatuses.contains(e.effect)) {
              out.buffs.push_back({Buff::On::kTarget, NormalizeStatus(e.effect), 0, e.duration});
            }
          }
        }
      }
      assume(estimate ? "the ability's shots land as the hit estimate says" : "every ability shot hits");
      out.parts.push_back({"Shot", damage, "RealmEye: Character Stats"});
    } else if (a.type == "BulletNova") {
      // Spells burst on the cursor; aimed at the target's center, every shot
      // starts inside it and hits once.
      const Projectile* p = shots.projectiles.empty() ? nullptr : &shots.projectiles[0];
      if (p == nullptr) {
        unsupported("BulletNova without a projectile");
        continue;
      }
      const int count = static_cast<int>(Param(a, "numShots")) + StatModCount(a, "statModNumShots", c);
      const double each =
          ExpectedAbilityDamage(p->min_damage, p->max_damage - 1, StatModBonus(a, "statModDamage", c), target,
                                p->armor_piercing);
      assume("spells are aimed at the target's center, so every shot hits");
      out.parts.push_back({"Spell burst, " + std::to_string(count) + " shots", count * each, "RealmEye: Spells"});
    } else if (a.type == "BulletCreate") {
      // Shots made around the cursor rather than fired from the player. On
      // wakizashis, the most common, the slash "is centered on the
      // crosshair" (RealmEye: Wakizashi). Some create another item's
      // projectile, named by `type` as a hex string.
      const Projectile* p = shots.projectiles.empty() ? nullptr : &shots.projectiles[0];
      if (const std::string type = a.Text("type"); !type.empty()) {
        const auto it = ability.created_projectiles.find(static_cast<int>(std::strtol(type.c_str(), nullptr, 0)));
        if (it == ability.created_projectiles.end()) {
          unsupported("BulletCreate shots of another projectile");
          continue;
        }
        p = &it->second;
      }
      if (p == nullptr) {
        unsupported("BulletCreate without a projectile");
        continue;
      }
      const int count = static_cast<int>(Param(a, "numShots")) + StatModCount(a, "statModNumShots", c);
      const double each =
          ExpectedAbilityDamage(p->min_damage, p->max_damage - 1, StatModBonus(a, "statModDamage", c), target,
                                p->armor_piercing);
      if (count > 0) {
        for (const ConditionEffect& e : p->condition_effects) {
          if (kTargetStatuses.contains(e.effect)) {
            out.buffs.push_back({Buff::On::kTarget, NormalizeStatus(e.effect), 0, e.duration});
          }
        }
      }
      assume("shots made at the cursor (like a wakizashi's slash) pass through the target, so every shot hits");
      out.parts.push_back({"Shots at the cursor, " + std::to_string(count), count * each, "RealmEye: Wakizashi"});
    } else if (a.type == "DamageNova") {
      const int count = static_cast<int>(Param(a, "activationCount")) + StatModCount(a, "statModActivationCount", c);
      const double each = ExpectedAbilityDamage(static_cast<int>(Param(a, "minDamage")),
                                                static_cast<int>(Param(a, "maxDamage")),
                                                StatModBonus(a, "statModDamage", c), target, false);
      assume("damage novas are aimed at the target and DEF applies to them");
      out.parts.push_back({"Damage nova, " + std::to_string(count) + " blasts", count * each, "RealmEye: Tomes"});
    } else if (a.type == "Lightning") {
      const double damage = LightningDamage(a, c);
      const double first = ExpectedAbilityDamage(0, 0, damage, target, false);
      assume("lightning's first target takes the full damage, and DEF applies to it");
      if (a.Number("aoeDamage")) {
        // RealmEye, Scepters: a struck enemy's shockblast "can deal damage to
        // other enemies".
        assume("a scepter's shockblast hits enemies around the one struck, not that one, so it adds nothing "
               "against a single target");
      }
      out.parts.push_back({"Lightning, first target", first, "RealmEye: Scepters"});
    } else if (a.type == "PoisonGrenade") {
      // Poison ignores DEF (RealmEye: Poisons).
      const double impact = Param(a, "impactDamage") + StatModBonus(a, "statModImpactDamage", c);
      const double total = Param(a, "totalDamage") + StatModBonus(a, "statModDamage", c);
      const double damage = (impact + total) * (target.cursed ? 1.25 : 1);
      assume("Curse adds 25% to poison damage too");
      if (a.Number("damageBoost")) {
        unsupported("poison's damage boost to poisoned targets");
      }
      out.parts.push_back({"Poison, impact and over time", damage, "RealmEye: Poisons"});
    } else if (a.type == "Trap") {
      // A trap thrown onto an enemy goes off as it lands (RealmEye: Traps).
      // One trap has an armorPierce flag, so the rest go through DEF.
      const int damage = static_cast<int>(Param(a, "totalDamage"));
      const double hit = ExpectedAbilityDamage(damage, damage, StatModBonus(a, "statModTrapDamage", c), target,
                                               a.Flag("armorPierce"));
      if (a.Number("bombCount")) {
        unsupported("trap bombs");
      }
      out.parts.push_back({"Trap", hit, "RealmEye: Traps"});
    } else if (a.type == "ConditionEffectSelf" || a.type == "ConditionEffectAura" || a.type == "StatBoostSelf" ||
               a.type == "StatBoostAura" || a.type == "EffectBlast") {
      if (const auto buff = BuffOf(a, c)) {
        out.buffs.push_back(*buff);
        if (buff->amount != 0 && a.Number("wisMin")) {
          unsupported("WIS scaling of stat boosts");
        }
      }
    } else if (!kNoDamageEffects.contains(a.type)) {
      unsupported(a.type);
    }
  }

  out.mp_cost = ability.mp_cost.value_or(0);
  out.cooldown_s = ability.cooldown.value_or(kDefaultCooldownS);
  if (!ability.cooldown) {
    assume("an ability with no cooldown set can be used every 0.5 seconds");
  }
  out.mp_per_second = MpPerSecond(c.wisdom);
  out.casts_per_second = 1 / std::max(out.cooldown_s, 1e-3);
  if (out.mp_cost > 0) {
    out.casts_per_second = std::min(out.casts_per_second, out.mp_per_second / out.mp_cost);
  }
  assume("the ability is used as often as MP regeneration and its cooldown allow, not counting the MP you start with");

  for (const AbilityPart& p : out.parts) {
    out.damage_per_cast += p.damage;
  }
  out.dps = out.damage_per_cast * out.casts_per_second;

  out.terms.push_back({"MP cost", out.mp_cost, Source::kClientData, "ability"});
  out.terms.push_back({"MP per second", out.mp_per_second, Source::kWiki, "RealmEye: Character Stats"});
  out.terms.push_back({"Cooldown", out.cooldown_s, ability.cooldown ? Source::kClientData : Source::kAssumed, ""});
  out.terms.push_back({"Casts per second", out.casts_per_second, Source::kWiki, "RealmEye: Character Stats"});
  return out;
}

}  // namespace rotmg
