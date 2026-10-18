#include "rotmg/ability.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>

#include "rotmg/damage.hpp"
#include "rotmg/hits.hpp"

namespace rotmg {

// Effect parameters by key, like a.Number(kCooldown).
using enum ParamKey;
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

double Param(const Activate& a, ParamKey key) { return a.Number(key).value_or(0); }

}  // namespace

double MpPerSecond(int wisdom) { return 0.5 + 0.12 * wisdom; }

// Undead are raised near the enemy rather than on it (Hugo, 2026-10-18),
// and aim at it, so shots spread wide, like Corrupted Souls' ring of five,
// can miss.
std::vector<int> UndeadHits(const Undead& undead, double radius) {
  std::vector<int> out;
  for (const Projectile& p : undead.projectiles) {
    if (undead.bursts > 0) {
      out.push_back(1);
      continue;
    }
    Weapon w;
    w.projectiles = {p};
    w.projectiles[0].id = 0;
    Subattack s;
    s.projectile_id = 0;
    s.num_projectiles = std::max(1, undead.shots);
    s.arc_gap = undead.arc_gap;
    out.push_back(HitModelFits(w) ? HitsPerVolley(w, s, kUndeadDistance, radius) : s.num_projectiles);
  }
  return out;
}

// RealmEye, Pet Abilities: the MP each heal restores and the delay between
// heals at the ends of each rarity's levels, from level 1 to 100. Levels in
// between are taken to move evenly from one end to the next, an assumption;
// the site only offers the ends.
double MagicHealMpPerSecond(int level) {
  struct Heal {
    int level;
    double mp;
    double delay_s;
  };
  constexpr std::array<Heal, 6> kHeals = {{{1, 1, 10}, {30, 3, 3.82}, {50, 8, 2.66}, {70, 17, 1.96}, {90, 33, 1.42}, {100, 45, 1}}};
  if (level < 1) {
    return 0;
  }
  level = std::min(level, kHeals.back().level);
  for (std::size_t i = 1; i < kHeals.size(); ++i) {
    if (level <= kHeals[i].level) {
      const Heal& a = kHeals[i - 1];
      const Heal& b = kHeals[i];
      const double t = static_cast<double>(level - a.level) / (b.level - a.level);
      return (a.mp + t * (b.mp - a.mp)) / (a.delay_s + t * (b.delay_s - a.delay_s));
    }
  }
  return kHeals[0].mp / kHeals[0].delay_s;
}

double AbilityCooldown(const Ability& ability) {
  return std::max(ability.cooldown.value_or(kMinCooldownS), kMinCooldownS);
}

namespace {

// Points of `stat` over `threshold`, or 0.
double Over(const Character& c, std::string_view stat, double threshold) {
  return stat.empty() ? 0 : std::max(0.0, c.Stat(stat) - threshold);
}

}  // namespace

double LightningDamage(const Activate& a, const Character& c) {
  return a.Number(kTotalDamage).value_or(0) +
         Over(c, "WIS", a.Number(kWisMin).value_or(50)) * a.Number(kWisDamageBase).value_or(0) / 10;
}

int LightningTargets(const Activate& a, const Character& c) {
  const double per = a.Number(kWisPerTarget).value_or(10);
  const double over = Over(c, "WIS", a.Number(kWisMin).value_or(50));
  return static_cast<int>(a.Number(kMaxTargets).value_or(1)) +
         (per > 0 ? static_cast<int>(std::floor(over / per + 1e-9)) : 0);
}

double ShockblastDamage(const Activate& a, const Character& c) {
  const double over = Over(c, a.Text(kScalingStat), a.Number(kStatModScalingMin).value_or(0));
  return a.Number(kAoeDamage).value_or(0) + over * a.Number(kStatModDamage).value_or(0) / 10;
}

int ShockblastTargets(const Activate& a, const Character& c) {
  const double per = a.Number(kStatModPerTarget).value_or(0);
  const double over = Over(c, a.Text(kScalingStat), a.Number(kStatModScalingMin).value_or(0));
  return static_cast<int>(a.Number(kAoeMaxTargets).value_or(1)) +
         (per > 0 ? static_cast<int>(std::floor(over / per + 1e-9)) : 0);
}

std::optional<Buff> BuffOf(const Activate& a, const Character& c) {
  if (a.type == "ConditionEffectSelf" || a.type == "ConditionEffectAura") {
    const std::string_view effect = a.Text(kEffect);
    if (kPlayerStatuses.contains(effect)) {
      return Buff{Buff::On::kPlayer, std::string(effect), 0, Param(a, kDuration) + StatModBonus(a, kStatModDuration, c)};
    }
  } else if (a.type == "StatBoostSelf" || a.type == "StatBoostAura") {
    const std::string_view stat = a.Text(kStat);
    if (stat == "ATT" || stat == "DEX") {
      return Buff{Buff::On::kPlayer, std::string(stat), static_cast<int>(Param(a, kAmount)), Param(a, kDuration)};
    }
  } else if (a.type == "EffectBlast") {
    // Duration grows by a second for every wisPerDuration WIS over wisMin
    // (RealmEye: Orbs gives "10 WIS -> +1 second" for wisPerDuration 10).
    const std::string_view effect = a.Text(kCondEffect);
    if (kTargetStatuses.contains(effect)) {
      double duration = Param(a, kCondDuration);
      const double per = Param(a, kWisPerDuration);
      const double over = c.wisdom - Param(a, kWisMin);
      if (per > 0 && over > 0) {
        duration += over / per;
      }
      return Buff{Buff::On::kTarget, NormalizeStatus(std::string(effect)), 0, duration};
    }
  } else if (a.type == "PoisonGrenade" && a.Number(kDamageBoost).value_or(1) > 1) {
    // RealmEye, Poisons: what you poison takes more damage from you, the
    // highest boost if several. Baneserpent Poison's page gives its 9.5%
    // (+0.225% per WIS over 50) as damageBoost 1.095 and
    // statModDamageBoost 0.00225. That it lasts as long as the poison is an
    // assumption.
    Buff boost{Buff::On::kTarget, kPoisonBoost, 0, Param(a, kDuration)};
    boost.factor = *a.Number(kDamageBoost) + StatModBonus(a, kStatModDamageBoost, c);
    return boost;
  }
  return std::nullopt;
}

double PoisonImpact(const Activate& a, const Character& c) {
  return Param(a, kImpactDamage) + StatModBonus(a, kStatModImpactDamage, c) / 10;
}

double PoisonTotal(const Activate& a, const Character& c) {
  return Param(a, kTotalDamage) + StatModBonus(a, kStatModDamage, c) / 10;
}

std::string DamageScaling(const Activate& effect) {
  for (const ParamKey key : {kStatModDamage, kStatModNumShots, kStatModImpactDamage, kStatModTrapDamage,
                             kStatModBombDamage, kStatModActivationCount}) {
    if (effect.Number(key).value_or(0) != 0 && !effect.Text(kScalingStat).empty()) {
      return std::string(effect.Text(kScalingStat));
    }
  }
  if (effect.type == "Lightning" && effect.Number(kWisDamageBase).value_or(0) != 0) {
    return "WIS";
  }
  return "";
}

double StatModBonus(const Activate& effect, ParamKey per_point_key, const Character& character) {
  const std::string_view stat = effect.Text(kScalingStat);
  const auto per_point = effect.Number(per_point_key);
  if (stat.empty() || !per_point) {
    return 0;
  }
  const double over = character.Stat(stat) - effect.Number(kStatModScalingMin).value_or(0);
  return over > 0 ? *per_point * over : 0;
}

int StatModCount(const Activate& effect, ParamKey per_point_key, const Character& character) {
  // A small epsilon: 0.0667 per point over 15 points is meant to be 1 shot,
  // not 0.9999... rounded down.
  return static_cast<int>(std::floor(StatModBonus(effect, per_point_key, character) + 1e-9));
}

bool GoesOffOnCast(const Activate& a) {
  const std::string_view required = a.Text(kRequiredConditions);
  return !a.Number(kHpMinThresholdRelative) && !a.Number(kIdMeter) && !a.Number(kMinimumMeter) &&
         !a.Number(kMeterCost) && (required.empty() || required == "In Combat");
}

double CastShare(const Activate& a, double casts_per_second) {
  return CastShare(std::clamp(a.Number(kProc).value_or(1), 0.0, 1.0), a.Number(kCooldown).value_or(0),
                   casts_per_second);
}

std::vector<double> HeldCastRates(const AbilityBreakdown& asap) {
  std::vector<double> out;
  for (const double cooldown : asap.blocked_cooldowns_s) {
    // CastShare's blocked casts: casting exactly n times per cooldown
    // blocks n - 1.
    const int blocked = static_cast<int>(std::ceil(cooldown * asap.casts_per_second - 1e-9)) - 1;
    for (int n = blocked; n >= 1; --n) {
      const double rate = n / cooldown;
      if (rate > 0 && rate < asap.casts_per_second) {
        out.push_back(rate);
      }
    }
  }
  std::ranges::sort(out, std::greater<>());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<double> CastRates(const AbilityBreakdown& asap) {
  std::vector<double> out = HeldCastRates(asap);
  for (const Buff& b : asap.buffs) {
    if (b.duration_s > 0 && 1 / b.duration_s < asap.casts_per_second) {
      out.push_back(1 / b.duration_s);
    }
  }
  if (kRarelyCasts < asap.casts_per_second) {
    out.push_back(kRarelyCasts);
  }
  std::ranges::sort(out, std::greater<>());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

double CastShare(double chance, double cooldown_s, double casts_per_second) {
  if (casts_per_second <= 0 || chance <= 0) {
    return 0;
  }
  // After it goes off, the casts within its cooldown can't set it off, then
  // each cast has the chance.
  const double blocked = std::max(0.0, std::ceil(cooldown_s * casts_per_second - 1e-9) - 1);
  return 1 / (blocked + 1 / chance);
}

AbilityBreakdown EvaluateAbility(const Ability& ability, const Character& character, const Target& target,
                                 const Assumptions& assumptions, double held_casts_per_second, bool details) {
  AbilityBreakdown out;
  if (details) {
    out.unsupported = ability.unsupported;
  }
  const Character c = character.With(ability.stat_bonuses);
  // The ability's own projectiles as a weapon, made only for effects that
  // fire them.
  std::optional<Weapon> made;
  const auto shots = [&]() -> const Weapon& {
    if (!made) {
      made = ability.AsWeapon();
    }
    return *made;
  };
  const auto unsupported = [&out, details](std::string_view what) {
    if (details && std::ranges::find(out.unsupported, what) == out.unsupported.end()) {
      out.unsupported.emplace_back(what);
    }
  };
  const auto assume = [&out, details](std::string_view what) {
    if (details && std::ranges::find(out.assumptions, what) == out.assumptions.end()) {
      out.assumptions.emplace_back(what);
    }
  };

  out.mp_cost = ability.mp_cost.value_or(0);
  out.cooldown_s = AbilityCooldown(ability);
  out.mp_per_second = MpPerSecond(c.wisdom) + c.pet_mp_per_second;
  out.casts_per_second = 1 / std::max(out.cooldown_s, 1e-3);
  if (out.mp_cost > 0) {
    out.casts_per_second = std::min(out.casts_per_second, out.mp_per_second / out.mp_cost);
  }

  // MP the gear gives back on casting lowers what a cast really costs, and
  // how often it comes back depends on how often you cast, so the two are
  // worked out together.
  ItemTypes worn_for_refunds = character.worn;
  worn_for_refunds.push_back(ability.type);
  for (const Activate& a : ability.on_ability) {
    if (a.type == "Magic" && GearWorn(a, worn_for_refunds) && GoesOffOnCast(a)) {
      out.refunds.push_back({Param(a, kAmount), std::clamp(a.Number(kProc).value_or(1), 0.0, 1.0),
                             a.Number(kCooldown).value_or(0)});
    }
  }
  const auto refund_at = [&](double rate) {
    double refund = 0;
    for (const MpRefund& r : out.refunds) {
      refund += r.amount * CastShare(r.chance, r.cooldown_s, rate);
    }
    return refund;
  };
  if (!out.refunds.empty() && out.mp_cost > 0) {
    // The rate MP allows when casting at `rate`, with what comes back at
    // that rate. Casting more often only blocks more refunds behind their
    // cooldowns, so it never rises with the rate, and the real rate is where
    // the two meet. Halving the interval finds it; feeding the rate back in
    // over and over can flip between two rates either side of a cooldown
    // step, and more WIS could then land on the lower one.
    const double most = 1 / std::max(out.cooldown_s, 1e-3);
    const auto allowed = [&](double rate) {
      const double cost = out.mp_cost - refund_at(rate);
      return cost > 0 ? std::min(most, out.mp_per_second / cost) : most;
    };
    double low = 0;
    double high = most;
    if (allowed(most) >= most) {
      low = most;
    }
    for (int halve = 0; halve < 60 && low < high; ++halve) {
      const double mid = (low + high) / 2;
      (allowed(mid) > mid ? low : high) = mid;
    }
    out.casts_per_second = low;
    out.mp_refund_per_cast = refund_at(low);
    assume("MP an item gives back on casting counts for the share of casts it comes back on");
  }
  // Casting slower never runs out of MP: refunds only come back on more of
  // the casts.
  if (held_casts_per_second > 0 && held_casts_per_second < out.casts_per_second) {
    out.casts_per_second = held_casts_per_second;
    out.mp_refund_per_cast = refund_at(held_casts_per_second);
    assume("the ability is cast only as often as helps the damage: held back so an item's effect on casting "
           "isn't blocked by its own cooldown, or less often when its own effects hurt more than they help");
  }

  // One effect of a cast. An item's effect that goes off on only a share of
  // the casts deals that share of its damage, and its buff is up for that
  // share of the time as if it started with every cast but ran shorter.
  double share = 1;
  double chance = 1;
  double cooldown = 0;
  std::string scaling;
  std::string prefix;
  const auto add_part = [&](AbilityPart p) {
    if (details) {
      p.name = prefix + p.name;
    }
    p.damage *= share;
    p.share = share;
    p.chance = chance;
    p.cooldown_s = cooldown;
    p.scaling = scaling;
    out.parts.push_back(std::move(p));
  };
  const auto add_buff = [&](Buff b) {
    if (share < 1) {
      const double every_s = 1 / (share * out.casts_per_second);
      b.duration_s = std::min(b.duration_s, every_s) * share;
    }
    out.buffs.push_back(b);
  };
  const auto apply = [&](const Activate& a) {
    scaling = DamageScaling(a);
    if (a.type == "Shoot") {
      // Fires the item's own projectiles at the target, like a weapon volley.
      const double bonus = StatModBonus(a, kStatModDamage, c);
      const bool estimate = assumptions.distance.has_value() && HitModelFits(shots());
      double damage = 0;
      for (const Subattack& s : shots().subattacks) {
        const Projectile* p = shots().FindProjectile(s.projectile_id);
        if (p == nullptr) {
          continue;
        }
        const int hits = estimate ? HitsPerVolley(shots(), s, *assumptions.distance, assumptions.target_radius)
                                  : s.num_projectiles;
        damage += hits * ExpectedAbilityDamage(p->min_damage, p->max_damage - 1, bonus, target, p->armor_piercing);
        if (hits > 0) {
          for (const ConditionEffect& e : p->condition_effects) {
            if (kTargetStatuses.contains(e.effect)) {
              add_buff({Buff::On::kTarget, NormalizeStatus(e.effect), 0, e.duration});
            }
          }
        }
      }
      assume(estimate ? "the ability's shots land as the hit estimate says" : "every ability shot hits");
      add_part({"Shot", damage, "RealmEye: Character Stats"});
    } else if (a.type == "BulletNova") {
      // Spells burst on the cursor; aimed at the target's center, every shot
      // starts inside it and hits once.
      const Projectile* p = ability.projectiles.empty() ? nullptr : &ability.projectiles[0];
      if (p == nullptr) {
        unsupported("BulletNova without a projectile");
        return;
      }
      const int count = static_cast<int>(Param(a, kNumShots)) + StatModCount(a, kStatModNumShots, c);
      const double each =
          ExpectedAbilityDamage(p->min_damage, p->max_damage - 1, StatModBonus(a, kStatModDamage, c), target,
                                p->armor_piercing);
      assume("spells are aimed at the target's center, so every shot hits");
      add_part({details ? "Spell burst, " + std::to_string(count) + " shots" : "", count * each, "RealmEye: Spells"});
    } else if (a.type == "BulletCreate") {
      // Shots made around the cursor rather than fired from the player. On
      // wakizashis, the most common, the slash "is centered on the
      // crosshair" (RealmEye: Wakizashi). Some create another item's
      // projectile, named by `type` as a hex string.
      const Projectile* p = ability.projectiles.empty() ? nullptr : &ability.projectiles[0];
      if (const std::string_view type = a.Text(kType); !type.empty()) {
        const auto it = ability.created_projectiles.find(static_cast<int>(std::strtol(std::string(type).c_str(), nullptr, 0)));
        if (it == ability.created_projectiles.end()) {
          unsupported("BulletCreate shots of another projectile");
          return;
        }
        p = &it->second;
      }
      if (p == nullptr) {
        unsupported("BulletCreate without a projectile");
        return;
      }
      const int count = static_cast<int>(Param(a, kNumShots)) + StatModCount(a, kStatModNumShots, c);
      const double each =
          ExpectedAbilityDamage(p->min_damage, p->max_damage - 1, StatModBonus(a, kStatModDamage, c), target,
                                p->armor_piercing);
      if (count > 0) {
        for (const ConditionEffect& e : p->condition_effects) {
          if (kTargetStatuses.contains(e.effect)) {
            add_buff({Buff::On::kTarget, NormalizeStatus(e.effect), 0, e.duration});
          }
        }
      }
      assume("shots made at the cursor (like a wakizashi's slash) pass through the target, so every shot hits");
      add_part({details ? "Shots at the cursor, " + std::to_string(count) : "", count * each, "RealmEye: Wakizashi"});
    } else if (a.type == "DamageNova") {
      const int count = static_cast<int>(Param(a, kActivationCount)) + StatModCount(a, kStatModActivationCount, c);
      const double each = ExpectedAbilityDamage(static_cast<int>(Param(a, kMinDamage)),
                                                static_cast<int>(Param(a, kMaxDamage)),
                                                StatModBonus(a, kStatModDamage, c), target, false);
      assume("damage novas are aimed at the target and DEF applies to them");
      add_part({details ? "Damage nova, " + std::to_string(count) + " blasts" : "", count * each, "RealmEye: Tomes"});
    } else if (a.type == "Lightning") {
      const double damage = LightningDamage(a, c);
      const double first = ExpectedAbilityDamage(0, 0, damage, target, false);
      assume("lightning's first target takes the full damage, and DEF applies to it");
      if (a.Number(kAoeDamage)) {
        // RealmEye, Scepters: a struck enemy's shockblast "can deal damage to
        // other enemies".
        assume("a scepter's shockblast hits enemies around the one struck, not that one, so it adds nothing "
               "against a single target");
      }
      add_part({"Lightning, first target", first, "RealmEye: Scepters"});
    } else if (a.type == "PoisonGrenade") {
      // Poison ignores DEF (RealmEye: Poisons).
      const double curse = target.cursed ? 1.25 : 1;
      const double impact = PoisonImpact(a, c) * curse;
      const double total = PoisonTotal(a, c) * curse;
      assume("Curse adds 25% to poison damage too");
      if (const auto boost = BuffOf(a, c)) {
        add_buff(*boost);
        assume("a poison's damage boost lasts as long as the poison and covers your other damage, not the poison's own");
      }
      if (impact > 0) {
        add_part({"Poison impact", impact, "RealmEye: Poisons"});
      }
      add_part({"Poison over time", total, "RealmEye: Poisons", Param(a, kDuration)});
    } else if (a.type == "Trap") {
      // A trap thrown onto an enemy goes off as it lands (RealmEye: Traps).
      // One trap has an armorPierce flag, so the rest go through DEF.
      const int damage = static_cast<int>(Param(a, kTotalDamage));
      const double hit = ExpectedAbilityDamage(damage, damage, StatModBonus(a, kStatModTrapDamage, c), target,
                                               a.Flag(kArmorPierce));
      add_part({"Trap", hit, "RealmEye: Traps"});
      // RealmEye, Giantcatcher Trap: a sticky bomb of bombDamage (+5 per WIS
      // over 34) goes off bombDelay after the trap, bombTriggerCount times.
      if (a.Number(kBombCount).value_or(0) > 0 && a.Number(kBombDamage)) {
        const double bomb_damage = Param(a, kBombDamage) + StatModBonus(a, kStatModBombDamage, c);
        const double bomb = ExpectedAbilityDamage(0, 0, bomb_damage, target, false);
        const double times = std::max(1.0, Param(a, kBombTriggerCount));
        assume("a trap's sticky bomb sticks to the target the trap hits and goes off on it");
        add_part({"Sticky bomb", times * bomb, "RealmEye: Giantcatcher Trap"});
      }
    } else if (a.type == "RaiseDead") {
      // Each undead attacks or bursts over its lifetime (Undead::Attacks),
      // its damage with the skull's StatMod bonus. At 1.5 attacks a second
      // that's 7 in 5 seconds, as RealmEye has Skull of Restless Souls'
      // skeletons shooting "up to 7 times".
      const auto it = ability.undead.find(a.Text(kUndeadId));
      if (it == ability.undead.end() || it->second.projectiles.empty()) {
        unsupported(a.type);
        return;
      }
      const Undead& u = it->second;
      const int raised = std::max(1, static_cast<int>(Param(a, kNumDead)));
      const int attacks = u.Attacks();
      const double bonus = StatModBonus(a, kStatModDamage, c);
      // Its statuses land again with every attack, so they're up from the
      // first until their duration after the last, and every attack after
      // the first lands on a target they're on, like Corrupted Souls' Curse.
      Target marked = target;
      for (const Projectile& p : u.projectiles) {
        for (const ConditionEffect& e : p.condition_effects) {
          if (kTargetStatuses.contains(e.effect)) {
            add_buff({Buff::On::kTarget, NormalizeStatus(e.effect), 0, u.lifetime_s + e.duration});
            const std::string status = NormalizeStatus(e.effect);
            marked.cursed |= status == "Curse";
            marked.exposed |= status == "Exposed";
            marked.armor_broken |= status == "Armor Broken";
          }
        }
      }
      const auto hit = [&](const Projectile& p, const Target& on) {
        return ExpectedAbilityDamage(p.min_damage, p.max_damage - 1, bonus, on, p.armor_piercing);
      };
      // A burst reaches the enemy if its radius does; RealmEye's are all
      // 2.25 tiles or more.
      const std::vector<int> landing = UndeadHits(u, assumptions.target_radius);
      double per_undead = 0;
      for (int k = 0; k < attacks; ++k) {
        const Target& on = k == 0 ? target : marked;
        if (u.bursts > 0) {
          const auto i = static_cast<std::size_t>(k);
          const double reach = u.burst_radii.empty() ? 0 : u.burst_radii[i % u.burst_radii.size()];
          if (kUndeadDistance <= reach + assumptions.target_radius) {
            per_undead += hit(u.projectiles[i % u.projectiles.size()], on);
          }
        } else {
          for (std::size_t i = 0; i < u.projectiles.size(); ++i) {
            per_undead += landing[i] * hit(u.projectiles[i], on);
          }
        }
      }
      assume("raised undead stand 1 tile from the target and aim at it, so only shots that pass through it hit");
      add_part({details ? (u.bursts > 0 ? "Undead, " + std::to_string(attacks) + " bursts"
                                        : "Undead, " + std::to_string(attacks) + " attacks")
                        : "",
                raised * per_undead, "RealmEye: " + ability.name, u.lifetime_s});
    } else if (a.type == "ConditionEffectSelf" || a.type == "ConditionEffectAura" || a.type == "StatBoostSelf" ||
               a.type == "StatBoostAura" || a.type == "EffectBlast") {
      if (const auto buff = BuffOf(a, c)) {
        add_buff(*buff);
        if (buff->amount != 0 && a.Number(kWisMin)) {
          unsupported("WIS scaling of stat boosts");
        }
      }
    } else if (!kNoDamageEffects.contains(a.type)) {
      unsupported(a.type);
    }
  };
  for (const Activate& a : ability.activates) {
    apply(a);
  }

  // Effects of the ability and the other gear that go off when it's used.
  ItemTypes worn = character.worn;
  worn.push_back(ability.type);
  prefix = "On use, ";
  for (const Activate& a : ability.on_ability) {
    // MP given back is counted above, in how often you can cast. A meter
    // filled only matters for switching modes.
    if (!GearWorn(a, worn) || a.type == "Magic" || a.type == "FillMeter") {
      continue;
    }
    if (!GoesOffOnCast(a)) {
      if (a.Number(kHpMinThresholdRelative)) {
        assume("you're at full HP, so item effects that need lower HP don't go off");
      } else {
        unsupported("on-use effects that need a meter or a status");
      }
      continue;
    }
    share = CastShare(a, out.casts_per_second);
    chance = std::clamp(a.Number(kProc).value_or(1), 0.0, 1.0);
    cooldown = a.Number(kCooldown).value_or(0);
    if (share <= 0) {
      continue;
    }
    if (cooldown > 0 && cooldown * out.casts_per_second > 1 + 1e-9) {
      out.blocked_cooldowns_s.push_back(cooldown);
    }
    if (share < 1) {
      assume("an item effect that can't go off on every cast counts for the share of casts it can, its buff up for "
             "that share of the time");
    }
    apply(a);
  }

  if (!ability.cooldown) {
    assume("an ability with no cooldown set can be used every 0.45 seconds, the shortest any ability allows");
  }
  assume("the ability is used as often as MP regeneration and its cooldown allow, not counting the MP you start with");
  if (c.pet_mp_per_second > 0) {
    assume("the pet's Magic Heal counts as a steady flow of MP that's always used");
  }

  for (const AbilityPart& p : out.parts) {
    out.damage_per_cast += p.damage;
  }
  out.dps = out.damage_per_cast * out.casts_per_second;

  if (details) {
    out.terms.push_back({"MP cost", out.mp_cost, Source::kClientData, "ability"});
    out.terms.push_back({"MP per second", out.mp_per_second, Source::kWiki, "RealmEye: Character Stats"});
    if (c.pet_mp_per_second > 0) {
      out.terms.push_back({"From the pet's Magic Heal", c.pet_mp_per_second, Source::kWiki, "RealmEye: Pet Abilities"});
    }
    out.terms.push_back({"Cooldown", out.cooldown_s, ability.cooldown ? Source::kClientData : Source::kAssumed, ""});
    out.terms.push_back({"Casts per second", out.casts_per_second, Source::kWiki, "RealmEye: Character Stats"});
  }
  return out;
}

}  // namespace rotmg
