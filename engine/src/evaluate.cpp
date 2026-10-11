#include "rotmg/evaluate.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <utility>

#include "rotmg/fire_rate.hpp"

namespace rotmg {
namespace {

// The longest gap between shots: 1.5 attacks per second at 0 DEX
// (RealmEye: Character Stats).
constexpr double kSlowestShotS = 1 / 1.5;

// On-shoot effects that can't change single-target DPS.
const std::set<std::string, std::less<>> kHarmlessSelfEffects = {"Slowed", "Sick"};

bool InCombatOnly(const Activate& a) {
  const std::string required = a.Text("requiredConditions");
  return required.empty() || required == "In Combat";
}

// A stat change on every shot that lasts longer than the gap between shots
// is always up while firing. Only ones that don't stack (channel
// noStackItm) and don't depend on HP qualify; Chain Dagger's, which stacks
// up to +10 (wiki), doesn't.
bool IsWhileShooting(const Activate& a) {
  const double duration = a.Number("duration").value_or(0);
  return a.type == "StatBoostSelf" && a.Number("proc").value_or(1) == 1 && a.Text("channel") == "noStackItm" &&
         InCombatOnly(a) && !a.Number("hpRequiredRelative") && !a.Number("hpMinThresholdRelative") &&
         duration >= a.Number("cooldown").value_or(0) && duration >= kSlowestShotS;
}

// A poison released on the first shot after each cooldown, with nothing to
// aim (Doku No Ken, Fire Blade).
bool IsPoisonBurst(const Activate& a) {
  return a.type == "PoisonGrenade" && a.Number("proc").value_or(1) == 1 && a.Text("requiredConditions").empty() &&
         a.Text("mustWear").empty() && !a.Flag("targetMouse") && !a.Number("damageBoost") && a.Number("cooldown");
}

}  // namespace

std::vector<StatBonus> WhileShooting(const Weapon& weapon) {
  std::vector<StatBonus> out;
  for (const Activate& a : weapon.on_shoot) {
    if (IsWhileShooting(a)) {
      out.push_back({a.Text("stat"), static_cast<int>(a.Number("amount").value_or(0))});
    }
  }
  return out;
}

int Character::Stat(std::string_view client_name) const {
  if (client_name == "ATT") return attack;
  if (client_name == "DEX") return dexterity;
  if (client_name == "WIS") return wisdom;
  if (client_name == "VIT") return vitality;
  if (client_name == "DEF") return defense;
  if (client_name == "SPD") return speed;
  if (client_name == "MAXHP") return max_hit_points;
  if (client_name == "MAXMP") return max_magic_points;
  return 0;
}

Character Character::With(const std::vector<StatBonus>& bonuses) const {
  Character out = *this;
  for (const StatBonus& b : bonuses) {
    if (b.stat == "ATT") out.attack += b.amount;
    if (b.stat == "DEX") out.dexterity += b.amount;
    if (b.stat == "WIS") out.wisdom += b.amount;
    if (b.stat == "VIT") out.vitality += b.amount;
    if (b.stat == "DEF") out.defense += b.amount;
    if (b.stat == "SPD") out.speed += b.amount;
    if (b.stat == "MAXHP") out.max_hit_points += b.amount;
    if (b.stat == "MAXMP") out.max_magic_points += b.amount;
  }
  return out;
}

Breakdown EvaluateWeapon(const Weapon& weapon, const Character& character, const Target& target,
                         const Assumptions& assumptions) {
  Breakdown out;
  out.unsupported = weapon.unsupported;

  const std::vector<StatBonus> while_shooting = WhileShooting(weapon);
  bool unmodelled_on_shoot = false;
  for (const Activate& a : weapon.on_shoot) {
    const bool harmless = a.type == "ConditionEffectSelf" && kHarmlessSelfEffects.contains(a.Text("effect"));
    if (!harmless && !IsWhileShooting(a) && !IsPoisonBurst(a)) {
      unmodelled_on_shoot = true;
    }
  }
  if (unmodelled_on_shoot && std::ranges::find(out.unsupported, "OnPlayerShootActivate") == out.unsupported.end()) {
    out.unsupported.push_back("OnPlayerShootActivate");
  }

  const Character geared = character.With(weapon.stat_bonuses).With(while_shooting);
  const int attack = geared.attack;
  const int dexterity = geared.dexterity;
  const Multiplier multiplier = DamageMultiplier(attack, character.status);
  for (const StatBonus& b : while_shooting) {
    out.terms.push_back(
        {b.stat + " change while shooting", static_cast<double>(b.amount), Source::kClientData, "weapon on-shoot effect"});
  }
  if (!while_shooting.empty()) {
    out.assumptions.push_back("you're in combat, so on-shoot stat changes that need it stay up while you fire");
  }
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

  // A burst goes off on the first shot after its cooldown, so the cooldown
  // rounds up to a whole number of shots at the weapon's fastest rate.
  double shots_per_second = 0;
  for (const SubattackResult& r : out.subattacks) {
    shots_per_second = std::max(shots_per_second, r.volleys_per_second);
  }
  for (const Activate& a : weapon.on_shoot) {
    if (!IsPoisonBurst(a) || shots_per_second <= 0) {
      continue;
    }
    const double shots_between = std::max(1.0, std::ceil(*a.Number("cooldown") * shots_per_second - 1e-9));
    ProcResult p;
    p.name = "Poison burst";
    p.per_second = shots_per_second / shots_between;
    // Poison ignores DEF, and every poison stacks (RealmEye: Poisons).
    p.damage = (a.Number("impactDamage").value_or(0) + a.Number("totalDamage").value_or(0)) * (target.cursed ? 1.25 : 1);
    p.dps = p.per_second * p.damage;
    p.reference = "RealmEye: Poisons";
    out.dps += p.dps;
    out.procs.push_back(p);
    out.assumptions.push_back("the target is inside every poison burst");
    if (target.cursed) {
      out.assumptions.push_back("Curse adds 25% to poison damage too");
    }
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
