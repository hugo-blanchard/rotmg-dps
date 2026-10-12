#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rotmg/evaluate.hpp"
#include "rotmg/model.hpp"

// Abilities: what one cast does, how often it can be cast, and the buffs and
// debuffs it keeps up. Rules come from the RealmEye wiki (Character Stats for
// StatMod and MP, the class ability pages for each kind of effect), with
// every number from the client.
namespace rotmg {

/// MP regenerated per second: 0.5 + 0.12 * WIS (RealmEye: Character Stats).
double MpPerSecond(int wisdom);

/// StatMod (RealmEye: Character Stats): `per_point` for every point of the
/// effect's scaling stat over its threshold, nothing below it. Reads the
/// scalingStat and statModScalingMin parameters.
double StatModBonus(const Activate& effect, std::string_view per_point_key, const Character& character);

/// The same for counts, like extra shots, which only grow in whole steps.
int StatModCount(const Activate& effect, std::string_view per_point_key, const Character& character);

/// A buff on the player or a debuff on the target that changes weapon DPS
/// while it lasts.
struct Buff {
  enum class On { kPlayer, kTarget };
  On on = On::kPlayer;
  /// A status effect (Damaging, Berserk, Weak, Dazed, Curse, Exposed, Armor
  /// Broken) or, with `amount`, a stat (ATT or DEX).
  std::string effect;
  int amount = 0;
  double duration_s = 0;
  /// For a damage boost on targets, what their damage taken is multiplied by.
  double factor = 1;
};

/// A scepter's lightning: damage on the first target, falling by decrDamage
/// on each next one, and how many targets it chains to. Both grow with WIS
/// over wisMin (50 if unset): damage by wisDamageBase / 10 a point, and one
/// more target every wisPerTarget points (10 if unset). These give the
/// per-WIS damage and targets in RealmEye's Scepters tables for every
/// scepter listed there.
double LightningDamage(const Activate& lightning, const Character& character);
int LightningTargets(const Activate& lightning, const Character& character);

/// The shockblast an enemy struck by lightning becomes (RealmEye: Scepters):
/// aoeActivationCount pulses, each hitting up to this many other enemies
/// within aoeRange for this damage. Damage grows by statModDamage / 10 a
/// point of the scaling stat over statModScalingMin, and targets by one every
/// statModPerTarget points, which matches the wiki's shockblast tables.
double ShockblastDamage(const Activate& lightning, const Character& character);
int ShockblastTargets(const Activate& lightning, const Character& character);

/// The buff or debuff one effect gives at these stats, if it changes weapon
/// DPS: Damaging, Berserk, Weak or Dazed, ATT or DEX boosts on the player,
/// Curse, Exposed or Armor Broken on targets, or a poison's damage boost on
/// what it poisons (kPoisonBoost).
std::optional<Buff> BuffOf(const Activate& effect, const Character& character);

/// A thrown poison's damage on impact and over its duration, before Curse.
/// Its StatMod amounts count per 10 points of the scaling stat: Baneserpent
/// Poison's statModImpactDamage 30 is "+3 per WIS over 50" (RealmEye:
/// Baneserpent Poison), Centipede Poison's 5 is "+0.5".
double PoisonImpact(const Activate& poison, const Character& character);
double PoisonTotal(const Activate& poison, const Character& character);

/// The name of a poison's damage boost as a Buff effect.
inline constexpr const char* kPoisonBoost = "Poison damage boost";

/// No ability can be used more often than this, in seconds, and it's the
/// cooldown of one that doesn't set its own.
inline constexpr double kMinCooldownS = 0.45;

/// The ability's cooldown in seconds, at least kMinCooldownS.
double AbilityCooldown(const Ability& ability);

/// Damage one kind of effect deals per cast.
struct AbilityPart {
  std::string name;
  double damage = 0;
  std::string reference;
  /// Seconds after the cast the damage is spread over evenly, like a
  /// poison's; 0 when it's dealt at once.
  double over_s = 0;
  /// For an item's effect that goes off on some casts only: its chance on a
  /// cast, its own cooldown in seconds, and the share of casts it goes off
  /// on in the long run, which `damage` already counts.
  double chance = 1;
  double cooldown_s = 0;
  double share = 1;
};

/// MP an item gives back when the ability is used, like Eden's Embrace's
/// 60% chance of +60 MP, at most every 8 seconds.
struct MpRefund {
  double amount = 0;
  double chance = 1;
  double cooldown_s = 0;
};

struct AbilityBreakdown {
  double mp_cost = 0;
  double cooldown_s = 0;
  double mp_per_second = 0;
  /// As often as MP and the cooldown allow, in the long run.
  double casts_per_second = 0;
  /// MP given back on casting, and what that comes to a cast in the long run.
  std::vector<MpRefund> refunds;
  double mp_refund_per_cast = 0;
  double damage_per_cast = 0;
  double dps = 0;
  std::vector<AbilityPart> parts;
  std::vector<Buff> buffs;
  std::vector<Term> terms;
  std::vector<std::string> assumptions;
  /// Effects and tags on this ability the engine doesn't model.
  std::vector<std::string> unsupported;
};

/// Whether an effect that goes off on using the ability can at full HP and
/// without a meter: not ones that need HP below a threshold
/// (hpMinThresholdRelative), a meter's charge, or a status other than being
/// in combat.
bool GoesOffOnCast(const Activate& effect);

/// The share of casts an on-use effect goes off on, from its own cooldown
/// and chance (proc).
double CastShare(const Activate& effect, double casts_per_second);

/// The same from an effect's chance and own cooldown in seconds.
double CastShare(double chance, double cooldown_s, double casts_per_second);

/// `character` includes every bonus but the ability's own, which are added.
/// The hit estimate settings in `assumptions` apply to abilities that fire
/// projectiles at the target. The ability's on-use effects count when the
/// gear they need is in `character.worn`.
AbilityBreakdown EvaluateAbility(const Ability& ability, const Character& character, const Target& target,
                                 const Assumptions& assumptions);

}  // namespace rotmg
