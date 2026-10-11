#include "rotmg/evaluate.hpp"

#include <cstddef>

#include "rotmg/fire_rate.hpp"

namespace rotmg {

Breakdown EvaluateWeapon(const Weapon& weapon, const Character& character, const Target& target,
                         const Assumptions& assumptions) {
  Breakdown out;
  out.unsupported = weapon.unsupported;

  const int attack = character.attack + weapon.StatBonusFor("ATT");
  const int dexterity = character.dexterity + weapon.StatBonusFor("DEX");
  const Multiplier multiplier = DamageMultiplier(attack, character.status);
  out.terms.push_back({"ATT with weapon", static_cast<double>(attack), Source::kClientData, "weapon stat bonuses"});
  out.terms.push_back({"DEX with weapon", static_cast<double>(dexterity), Source::kClientData, "weapon stat bonuses"});
  out.terms.push_back({"Damage multiplier", multiplier.Value(), Source::kWiki, "RealmEye: Character Stats"});
  out.terms.push_back({"Attacks per second", AttacksPerSecond(dexterity, character.status), Source::kWiki,
                       "RealmEye: Character Stats"});
  out.terms.push_back({"Target DEF", static_cast<double>(EffectiveDefense(target, false)), Source::kWiki,
                       "RealmEye: Status Effects"});

  bool all_hit_assumed = false;
  bool has_burst = false;
  for (std::size_t i = 0; i < weapon.subattacks.size(); ++i) {
    const Subattack& s = weapon.subattacks[i];
    const Projectile* projectile = weapon.FindProjectile(s.projectile_id);
    if (projectile == nullptr) {
      continue;  // the pipeline rejects these; only a hand-edited dataset gets here
    }
    SubattackResult r;
    r.projectile_id = s.projectile_id;
    r.volleys_per_second = VolleysPerSecond(s, dexterity, character.status);
    r.damage_per_hit = ExpectedHitDamage(*projectile, multiplier, target);
    if (i < assumptions.hits_per_volley.size() && assumptions.hits_per_volley[i]) {
      r.hits_per_volley = *assumptions.hits_per_volley[i];
      out.terms.push_back({"Subattack " + std::to_string(i + 1) + " hits per volley", r.hits_per_volley, Source::kUser, ""});
    } else {
      r.hits_per_volley = s.num_projectiles;
      all_hit_assumed = true;
    }
    if (s.burst_count && s.burst_delay) {
      has_burst = true;
      out.terms.push_back({"Subattack " + std::to_string(i + 1) + " burst delay",
                           BurstDelay(s, dexterity, character.status), Source::kWiki,
                           "RealmEye: Weapon Attributes"});
    }
    r.dps = r.volleys_per_second * r.hits_per_volley * r.damage_per_hit;
    out.dps += r.dps;
    out.subattacks.push_back(r);
  }

  out.assumptions.push_back("damage after DEF and Curse is not rounded");
  if (all_hit_assumed) {
    out.assumptions.push_back("every projectile hits");
  }
  if (target.armored && target.exposed) {
    out.assumptions.push_back("Armored applies before Exposed");
  }
  if (has_burst && character.status.berserk && !character.status.dazed) {
    out.assumptions.push_back("Berserk multiplies burst delay by 0.67");
  }
  return out;
}

}  // namespace rotmg
