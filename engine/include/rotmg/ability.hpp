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
};

/// A scepter's lightning: damage on the first target, falling by decrDamage
/// on each next one, and how many targets it chains to. Both grow with WIS
/// over wisMin (50 if unset): damage by wisDamageBase / 10 a point, and one
/// more target every wisPerTarget points (10 if unset). These give the
/// per-WIS damage and targets in RealmEye's Scepters tables for every
/// scepter listed there.
double LightningDamage(const Activate& lightning, const Character& character);
int LightningTargets(const Activate& lightning, const Character& character);

/// The buff or debuff one effect gives at these stats, if it changes weapon
/// DPS: Damaging, Berserk, Weak or Dazed, ATT or DEX boosts on the player,
/// Curse, Exposed or Armor Broken on targets.
std::optional<Buff> BuffOf(const Activate& effect, const Character& character);

/// The ability's default cooldown when it doesn't set one, in seconds. The
/// client's default isn't documented.
inline constexpr double kDefaultCooldownS = 0.5;

/// Damage one kind of effect deals per cast.
struct AbilityPart {
  std::string name;
  double damage = 0;
  std::string reference;
};

struct AbilityBreakdown {
  double mp_cost = 0;
  double cooldown_s = 0;
  double mp_per_second = 0;
  /// As often as MP and the cooldown allow, in the long run.
  double casts_per_second = 0;
  double damage_per_cast = 0;
  double dps = 0;
  std::vector<AbilityPart> parts;
  std::vector<Buff> buffs;
  std::vector<Term> terms;
  std::vector<std::string> assumptions;
  /// Effects and tags on this ability the engine doesn't model.
  std::vector<std::string> unsupported;
};

/// `character` includes every bonus but the ability's own, which are added.
/// The hit estimate settings in `assumptions` apply to abilities that fire
/// projectiles at the target.
AbilityBreakdown EvaluateAbility(const Ability& ability, const Character& character, const Target& target,
                                 const Assumptions& assumptions);

}  // namespace rotmg
