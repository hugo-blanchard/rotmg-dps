#include "rotmg/evaluate.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

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

  const std::vector<std::string> gaps = HitModelGaps(weapon);
  const bool estimate = assumptions.distance.has_value() && gaps.empty();
  bool all_hit_assumed = false;
  bool estimated = false;
  bool has_burst = false;
  std::vector<std::pair<std::size_t, double>> burst_delays;
  for (std::size_t i = 0; i < weapon.subattacks.size(); ++i) {
    const Subattack& s = weapon.subattacks[i];
    const Projectile* projectile = weapon.FindProjectile(s.projectile_id);
    if (projectile == nullptr) {
      continue;  // the pipeline rejects these; only a hand-edited dataset gets here
    }
    SubattackResult r;
    r.projectile_id = s.projectile_id;
    r.projectiles_per_volley = s.num_projectiles;
    r.volleys_per_second = VolleysPerSecond(s, dexterity, character.status);
    r.damage_per_hit = ExpectedHitDamage(*projectile, multiplier, target);
    const auto set = [i](const std::vector<std::optional<double>>& values) {
      return i < values.size() && values[i].has_value();
    };
    if (set(assumptions.hits_per_volley)) {
      r.hits_per_volley = *assumptions.hits_per_volley[i];
      r.hits_source = Source::kUser;
    } else if (set(assumptions.curated_hits_per_volley)) {
      r.hits_per_volley = *assumptions.curated_hits_per_volley[i];
      r.hits_source = Source::kCurated;
    } else if (estimate) {
      r.hits_per_volley = HitsPerVolley(weapon, s, *assumptions.distance, assumptions.target_radius);
      r.hits_source = Source::kGeometricEstimate;
      estimated = true;
    } else {
      r.hits_per_volley = s.num_projectiles;
      all_hit_assumed = true;
    }
    if (s.burst_count && s.burst_delay) {
      has_burst = true;
      burst_delays.push_back({i, BurstDelay(s, dexterity, character.status)});
    }
    r.dps = r.volleys_per_second * r.hits_per_volley * r.damage_per_hit;
    out.dps += r.dps;
    out.subattacks.push_back(r);
  }

  // Most burst weapons share one delay across subattacks; list it once then.
  const bool one_delay = std::ranges::all_of(
      burst_delays, [&](const auto& d) { return d.second == burst_delays.front().second; });
  for (const auto& [i, delay] : burst_delays) {
    const std::string name = one_delay ? "Burst delay" : "Subattack " + std::to_string(i + 1) + " burst delay";
    out.terms.push_back({name, delay, Source::kWiki, "RealmEye: Weapon Attributes"});
    if (one_delay) {
      break;
    }
  }

  out.assumptions.push_back("damage after DEF and Curse is not rounded");
  if (all_hit_assumed) {
    if (assumptions.distance && !gaps.empty()) {
      std::string list;
      for (const std::string& gap : gaps) {
        list += (list.empty() ? "" : " and ") + gap;
      }
      out.assumptions.push_back("every projectile hits, since the hit estimate can't follow " + list);
    } else {
      out.assumptions.push_back("every projectile hits");
    }
  }
  if (estimated) {
    out.assumptions.push_back("the hit estimate aims perfectly at a target that doesn't move");
    for (std::string& a : HitModelAssumptions(weapon)) {
      out.assumptions.push_back(std::move(a));
    }
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
