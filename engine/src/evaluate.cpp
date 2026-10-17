#include "rotmg/evaluate.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include "rotmg/ability.hpp"
#include "rotmg/fire_rate.hpp"

namespace rotmg {

// Effect parameters by key, like a.Number(kCooldown).
using enum ParamKey;
namespace {

// The longest gap between shots: 1.5 attacks per second at 0 DEX
// (RealmEye: Character Stats).
constexpr double kSlowestShotS = 1 / 1.5;

// On-shoot effects that can't change single-target DPS.
const std::set<std::string, std::less<>> kHarmlessSelfEffects = {"Slowed", "Sick"};

bool InCombatOnly(const Activate& a) {
  const std::string_view required = a.Text(kRequiredConditions);
  return required.empty() || required == "In Combat";
}

// A stat change on every shot that lasts longer than the gap between shots
// is always up while firing. Only ones that don't stack (channel
// noStackItm) and don't depend on HP qualify; Chain Dagger's, which stacks
// up to +10 (wiki), doesn't.
bool IsWhileShooting(const Activate& a) {
  const double duration = a.Number(kDuration).value_or(0);
  return a.type == "StatBoostSelf" && a.Number(kProc).value_or(1) == 1 && a.Text(kChannel) == "noStackItm" &&
         InCombatOnly(a) && !a.Number(kHpRequiredRelative) && !a.Number(kHpMinThresholdRelative) &&
         duration >= a.Number(kCooldown).value_or(0) && duration >= kSlowestShotS;
}

// A poison released on the first shot after each cooldown, with nothing to
// aim (Doku No Ken, Fire Blade).
bool IsPoisonBurst(const Activate& a) {
  return a.type == "PoisonGrenade" && a.Number(kProc).value_or(1) == 1 && a.Text(kRequiredConditions).empty() &&
         a.Text(kMustWear).empty() && !a.Flag(kTargetMouse) && !a.Number(kDamageBoost) && a.Number(kCooldown);
}

// Shots from one on-shoot proc going off to the next, for a proc with a
// cooldown. The shot after the cooldown ends comes half a shot later on
// average: weapons fire fast enough that their shots don't line up with a
// cooldown on purpose, unlike an ability's casts (Hugo, 2026-10-12).
double ShotsPerCycle(double cooldown_s, double shots_per_second) {
  return std::max(1.0, cooldown_s * shots_per_second + 0.5);
}

// A number as strtol reads it with base 0: leading spaces, a sign, then hex
// after 0x, octal after a leading 0, decimal otherwise, up to the first
// character that isn't a digit; 0 when there are none.
int ParseType(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  bool negative = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
  int base = 10;
  if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X') && i + 2 < s.size() &&
      std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
    base = 16;
    i += 2;
  } else if (i < s.size() && s[i] == '0') {
    base = 8;
  }
  long value = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    const int digit = c >= '0' && c <= '9'   ? c - '0'
                      : c >= 'a' && c <= 'z' ? c - 'a' + 10
                      : c >= 'A' && c <= 'Z' ? c - 'A' + 10
                                             : 99;
    if (digit >= base) break;
    value = value * base + digit;
  }
  return static_cast<int>(negative ? -value : value);
}

// Whether `test` holds for every item type in a ';'-separated list (any,
// with `any`).
template <typename Test>
bool EachType(std::string_view list, bool any, const Test& test) {
  std::size_t start = 0;
  while (start < list.size()) {
    const std::size_t end = std::min(list.find(';', start), list.size());
    if (test(ParseType(list.substr(start, end - start))) == any) return any;
    start = end + 1;
  }
  return !any;
}

// Extra shots made at the cursor on every shot past a cooldown, like
// Marionette Ravager's or a Syndicate Gear ring's. Novas around the player
// (Wand of Geb), meter-gated ones (Staff of Esben) and ones that need a
// status on you (Lethal Strike, Curse) aren't modelled.
bool IsProcShot(const Weapon& w, const Activate& a) {
  if (a.type != "BulletCreate" || a.Number(kProc).value_or(1) != 1 || !a.Flag(kTargetMouse) ||
      !a.Text(kRequiredConditions).empty() || a.Number(kIdMeter) || a.Number(kMeterCost)) {
    return false;
  }
  const std::string_view type = a.Text(kType);
  return type.empty() ? !w.projectiles.empty()
                      : w.created_projectiles.contains(ParseType(type));
}

}  // namespace

bool IsChanceBoost(const Activate& a) {
  const double proc = a.Number(kProc).value_or(1);
  return a.type == "StatBoostSelf" && proc > 0 && proc < 1 && a.Number(kDuration).value_or(0) > 0 &&
         InCombatOnly(a) && !a.Number(kHpRequiredRelative) && !a.Number(kHpMinThresholdRelative);
}

// Wand of Geb's needs all four set pieces, matching its tooltip's "With Full
// Set"; the Alien enchantments' boosts need any one Alien item.
bool GearWorn(const Activate& a, std::span<const int> worn) {
  const auto wearing = [&](int type) { return std::ranges::find(worn, type) != worn.end(); };
  const auto not_wearing = [&](int type) { return !wearing(type); };
  const std::string_view any = a.Text(kMustWearOR);
  return EachType(a.Text(kMustWear), false, wearing) && (any.empty() || EachType(any, true, wearing)) &&
         EachType(a.Text(kMustNotWear), false, not_wearing) && EachType(a.Text(kMustNotWearOR), false, not_wearing);
}

OnShootModel ModelOf(const Weapon& weapon, const Activate& a, std::span<const int> worn) {
  if (IsWhileShooting(a)) {
    return OnShootModel::kWhileShooting;
  }
  if (IsPoisonBurst(a)) {
    return OnShootModel::kPoisonBurst;
  }
  if (!GearWorn(a, worn)) {
    return OnShootModel::kNone;
  }
  if (IsProcShot(weapon, a)) {
    return OnShootModel::kExtraShots;
  }
  return IsChanceBoost(a) ? OnShootModel::kChanceBoost : OnShootModel::kNone;
}

bool CountsAsChanceBoost(const Weapon& weapon, const Activate& boost, std::span<const int> worn) {
  const auto no_stack = [](const Activate& a) { return a.Text(kChannel).starts_with("noStack"); };
  if (!no_stack(boost)) {
    return true;
  }
  // Which of two non-stacking boosts of a stat wins, as a tuple compared in
  // order; which one the game honours isn't documented.
  const auto rank = [](const Activate& a) {
    return std::tuple{a.Number(kAmount).value_or(0), a.Number(kDuration).value_or(0), a.Number(kProc).value_or(0)};
  };
  bool earlier = true;
  for (const Activate& other : weapon.on_shoot) {
    if (&other == &boost) {
      earlier = false;
      continue;
    }
    if (!IsChanceBoost(other) || !no_stack(other) || other.Text(kStat) != boost.Text(kStat) ||
        !GearWorn(other, worn)) {
      continue;
    }
    if (rank(other) > rank(boost) || (earlier && rank(other) == rank(boost))) {
      return false;
    }
  }
  return true;
}

std::vector<ChanceBoost> ChanceBoosts(const Weapon& weapon, std::span<const int> worn, double shots_per_second) {
  std::vector<ChanceBoost> out;
  ItemTypes wearing(worn);
  wearing.push_back(weapon.type);
  for (const Activate& a : weapon.on_shoot) {
    if (!IsChanceBoost(a) || !GearWorn(a, wearing) || !CountsAsChanceBoost(weapon, a, wearing) ||
        shots_per_second <= 0) {
      continue;
    }
    const double duration = *a.Number(kDuration);
    const double cycle = a.Number(kCooldown).value_or(0) + 1 / (*a.Number(kProc) * shots_per_second);
    out.push_back({{std::string(a.Text(kStat)), static_cast<int>(a.Number(kAmount).value_or(0))},
                   duration,
                   std::min(1.0, duration / cycle)});
  }
  return out;
}

std::vector<StatBonus> WhileShooting(const Weapon& weapon) {
  std::vector<StatBonus> out;
  for (const Activate& a : weapon.on_shoot) {
    if (IsWhileShooting(a)) {
      out.push_back({std::string(a.Text(kStat)), static_cast<int>(a.Number(kAmount).value_or(0))});
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

namespace {

// The weapon's DPS, subattacks and procs and, with `details`, everything
// that explains them. The optimizer and the buff windows of a build only
// need the numbers, and building the explanation's text takes most of the
// time.
Breakdown Evaluate(const Weapon& weapon, const Character& character, const Target& target,
                   const Assumptions& assumptions, bool details) {
  Breakdown out;
  if (details) {
    out.unsupported = weapon.unsupported;
  }

  const std::vector<StatBonus> while_shooting = WhileShooting(weapon);
  bool unmodelled_on_shoot = false;
  ItemTypes worn = character.worn;
  worn.push_back(weapon.type);
  // Only the explanation lists effects that aren't modelled.
  if (details) {
    for (const Activate& a : weapon.on_shoot) {
      const bool harmless = a.type == "ConditionEffectSelf" && kHarmlessSelfEffects.contains(a.Text(kEffect));
      // An effect for gear that isn't worn doesn't go off, so it can't be missing.
      const bool off = !GearWorn(a, worn);
      if (!harmless && !off && !IsWhileShooting(a) && !IsPoisonBurst(a) && !IsProcShot(weapon, a) &&
          !IsChanceBoost(a)) {
        unmodelled_on_shoot = true;
      }
    }
  }
  if (details && unmodelled_on_shoot &&
      std::ranges::find(out.unsupported, "OnPlayerShootActivate") == out.unsupported.end()) {
    out.unsupported.push_back("OnPlayerShootActivate");
  }

  const Character geared = character.With(weapon.stat_bonuses).With(while_shooting);
  const int attack = geared.attack;
  const int dexterity = geared.dexterity;
  const Multiplier multiplier = WithDamageBonus(DamageMultiplier(attack, character.status), weapon.damage_bonus);
  if (details) {
    for (const StatBonus& b : while_shooting) {
      out.terms.push_back({b.stat + " change while shooting", static_cast<double>(b.amount), Source::kClientData,
                           "weapon on-shoot effect"});
    }
    if (!while_shooting.empty()) {
      out.assumptions.push_back("you're in combat, so on-shoot stat changes that need it stay up while you fire");
    }
    out.terms.push_back({"ATT with weapon", static_cast<double>(attack), Source::kClientData, "weapon stat bonuses"});
    out.terms.push_back(
        {"DEX with weapon", static_cast<double>(dexterity), Source::kClientData, "weapon stat bonuses"});
    out.terms.push_back({"Damage multiplier", multiplier.Value(), Source::kWiki, "RealmEye: Character Stats"});
    out.terms.push_back({"Attacks per second", AttacksPerSecond(dexterity, character.status), Source::kWiki,
                         "RealmEye: Character Stats"});
    out.terms.push_back({"Target DEF", static_cast<double>(EffectiveDefense(target, false)), Source::kWiki,
                         "RealmEye: Status Effects"});
  }

  const bool estimate = assumptions.distance.has_value() && HitModelFits(weapon);
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

  // A burst goes off on the first shot after its cooldown, at the weapon's
  // fastest rate (see ShotsPerCycle).
  double shots_per_second = 0;
  for (const SubattackResult& r : out.subattacks) {
    shots_per_second = std::max(shots_per_second, r.volleys_per_second);
  }
  for (const Activate& a : weapon.on_shoot) {
    if (!IsPoisonBurst(a) || shots_per_second <= 0) {
      continue;
    }
    ProcResult p;
    p.name = "Poison burst";
    p.per_second = shots_per_second / ShotsPerCycle(*a.Number(kCooldown), shots_per_second);
    // Poison ignores DEF, and every poison stacks (RealmEye: Poisons).
    p.damage = (a.Number(kImpactDamage).value_or(0) + a.Number(kTotalDamage).value_or(0)) * (target.cursed ? 1.25 : 1);
    p.dps = p.per_second * p.damage;
    p.reference = "RealmEye: Poisons";
    out.dps += p.dps;
    out.procs.push_back(p);
    if (details) {
      out.assumptions.push_back("the target is inside every poison burst");
      if (target.cursed) {
        out.assumptions.push_back("Curse adds 25% to poison damage too");
      }
    }
  }

  // Extra shots at the cursor, one row per projectile and cooldown. Like
  // abilities, they don't scale with ATT; only a scalingStat in the data
  // scales them, and procs without one don't scale at all (Divinity's is
  // known not to).
  bool any_shots = false;
  for (const Activate& a : weapon.on_shoot) {
    if (!IsProcShot(weapon, a) || !GearWorn(a, worn) || shots_per_second <= 0) {
      continue;
    }
    const std::string_view type = a.Text(kType);
    const Projectile& p =
        type.empty() ? weapon.projectiles[0]
                     : weapon.created_projectiles.at(ParseType(type));
    const double cooldown = a.Number(kCooldown).value_or(0);
    const double shots_between = ShotsPerCycle(cooldown, shots_per_second);
    const int count = static_cast<int>(a.Number(kNumShots).value_or(1)) + StatModCount(a, kStatModNumShots, geared);
    const double each = ExpectedAbilityDamage(p.min_damage, p.max_damage - 1, StatModBonus(a, kStatModDamage, geared),
                                              target, p.armor_piercing);
    out.dps += shots_per_second / shots_between * count * each;
    any_shots = true;
    if (!details) {
      out.procs.push_back({"", shots_per_second / shots_between, count * each,
                           shots_per_second / shots_between * count * each, "", DamageScaling(a)});
      continue;
    }
    const std::string name = "Extra shots (" + p.object_id + ")";
    auto row = std::ranges::find_if(out.procs, [&](const ProcResult& r) {
      return r.name == name && r.per_second == shots_per_second / shots_between;
    });
    if (row == out.procs.end()) {
      out.procs.push_back({name, shots_per_second / shots_between, 0, 0, "", DamageScaling(a)});
      row = out.procs.end() - 1;
    }
    row->damage += count * each;
    row->dps = row->per_second * row->damage;
  }
  if (!details) {
    return out;
  }
  if (any_shots) {
    out.assumptions.push_back("extra shots made at the cursor pass through the target, so every one hits");
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
    const std::vector<std::string> gaps = HitModelGaps(weapon);
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
  if (weapon.damage_bonus != 1) {
    out.assumptions.push_back("an enchantment's damage bonus multiplies each roll along with ATT");
  }
  if (target.armored && target.exposed) {
    out.assumptions.push_back("Armored applies before Exposed");
  }
  if (has_burst && character.status.berserk && !character.status.dazed) {
    out.assumptions.push_back("Berserk multiplies burst delay by 0.67");
  }
  return out;
}

}  // namespace

Breakdown EvaluateWeapon(const Weapon& weapon, const Character& character, const Target& target,
                         const Assumptions& assumptions) {
  return Evaluate(weapon, character, target, assumptions, true);
}

Breakdown EvaluateWeaponNumbers(const Weapon& weapon, const Character& character, const Target& target,
                                const Assumptions& assumptions) {
  return Evaluate(weapon, character, target, assumptions, false);
}

double EvaluateWeaponDps(const Weapon& weapon, const Character& character, const Target& target,
                         const Assumptions& assumptions) {
  return EvaluateWeaponNumbers(weapon, character, target, assumptions).dps;
}

}  // namespace rotmg
