#include "rotmg/build.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rotmg {

// Effect parameters by key, like a.Number(kCooldown).
using enum ParamKey;
namespace {

// Puts a set of buffs and debuffs up, on top of whatever the player chose.
void Apply(const std::vector<const Buff*>& active, Character& character, Target& target) {
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
}

// A DPS, and the part of it from sources that scale with the stat asked
// about (DpsWeights::stat), 0 when none is.
struct Dps {
  double total = 0;
  double stat = 0;
};

// Whether the weapon's shots count for `stat`: ATT raises their damage and
// DEX their rate.
bool ShotsScaleWith(std::string_view stat) { return stat == "ATT" || stat == "DEX"; }

bool ProcScalesWith(std::string_view scaling, std::string_view stat) {
  return scaling == stat || (scaling == kScalesLikeShots && ShotsScaleWith(stat));
}

// The weapon's DPS with a set of buffs and debuffs up.
Dps WeaponDps(const Weapon& weapon, Character character, Target target, const Assumptions& assumptions,
              const std::vector<const Buff*>& active, std::string_view stat) {
  Apply(active, character, target);
  if (stat.empty()) {
    return {EvaluateWeaponDps(weapon, character, target, assumptions), 0};
  }
  const Breakdown b = EvaluateWeaponNumbers(weapon, character, target, assumptions);
  Dps out{b.dps, 0};
  for (const SubattackResult& s : b.subattacks) {
    if (ShotsScaleWith(stat)) out.stat += s.dps;
  }
  for (const ProcResult& p : b.procs) {
    if (ProcScalesWith(p.scaling, stat)) out.stat += p.dps;
  }
  return out;
}

// The ability's DPS from the parts that scale with `stat`.
double AbilityStatDps(const AbilityBreakdown& a, std::string_view stat) {
  if (stat.empty()) {
    return 0;
  }
  double per_cast = 0;
  for (const AbilityPart& p : a.parts) {
    if (p.scaling == stat) per_cast += p.damage;
  }
  return per_cast * a.casts_per_second;
}

// The mixes of the weapon's chance boosts, each up or not on its own, as
// the character each makes and how often it comes up. Boosts of stats the
// weapon doesn't read leave it as it is, so they're left out, and mixes
// that come to the same stats come back as one: ATT and DEX count, and any
// stat an on-shoot effect scales with.
std::vector<std::pair<double, Character>> BoostMixes(const Weapon& weapon, const Character& character,
                                                     const std::vector<ChanceBoost>& all_boosts) {
  std::vector<std::string> read = {"ATT", "DEX"};
  for (const Activate& a : weapon.on_shoot) {
    if (const std::string_view stat = a.Text(kScalingStat); !stat.empty() && std::ranges::find(read, stat) == read.end()) {
      read.emplace_back(stat);
    }
  }
  std::vector<ChanceBoost> boosts;
  for (const ChanceBoost& b : all_boosts) {
    if (std::ranges::find(read, b.bonus.stat) != read.end()) {
      boosts.push_back(b);
    }
  }
  std::map<std::vector<int>, std::size_t> seen;
  std::vector<std::pair<double, Character>> out;
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
    if (weight <= 0) {
      continue;
    }
    std::vector<int> key;
    for (const std::string& stat : read) {
      key.push_back(c.Stat(stat));
    }
    if (auto it = seen.find(key); it != seen.end()) {
      out[it->second].first += weight;
    } else {
      seen.emplace(key, out.size());
      out.emplace_back(weight, c);
    }
  }
  return out;
}

// The weapon's DPS averaged over its chance boosts, with whatever else is
// up.
Dps MixedDps(const Weapon& weapon, const Character& character, const Target& target,
             const Assumptions& assumptions, const std::vector<const Buff*>& active,
             const std::vector<ChanceBoost>& boosts, std::string_view stat) {
  Dps total;
  for (const auto& [weight, c] : BoostMixes(weapon, character, boosts)) {
    const Dps d = WeaponDps(weapon, c, target, assumptions, active, stat);
    total.total += weight * d.total;
    total.stat += weight * d.stat;
  }
  return total;
}

void WithAbility(BuildBreakdown& out, const Weapon& weapon, const Character& with_ability_gear, const Target& target,
                 const Assumptions& assumptions, const std::vector<ChanceBoost>& boosts, std::string_view stat);

// The weapon's DPS averaged over a cast cycle of the ability, with its
// buffs up for their part of each cycle and `no_buffs` for the rest.
Dps CycleWeaponDps(const AbilityBreakdown& a, Dps no_buffs, const Weapon& weapon, const Character& with_ability_gear,
                   const Target& target, const Assumptions& assumptions, const std::vector<ChanceBoost>& boosts,
                   std::string_view stat) {
  if (a.buffs.empty() || a.casts_per_second <= 0) {
    return no_buffs;
  }
  const double period = 1 / a.casts_per_second;
  std::vector<double> lasts;
  for (const Buff& b : a.buffs) {
    lasts.push_back(std::clamp(b.duration_s, 0.0, period));
  }
  std::vector<double> ends = lasts;
  std::ranges::sort(ends);
  ends.erase(std::unique(ends.begin(), ends.end()), ends.end());

  double start = 0;
  Dps weighted;
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
    const Dps d = MixedDps(weapon, with_ability_gear, target, assumptions, active, boosts, stat);
    weighted.total += (end - start) * d.total;
    weighted.stat += (end - start) * d.stat;
    start = end;
  }
  if (start < period) {
    weighted.total += (period - start) * no_buffs.total;
    weighted.stat += (period - start) * no_buffs.stat;
  }
  return {weighted.total / period, weighted.stat / period};
}

}  // namespace

namespace {

// What every evaluation of a build shares: the character with the
// ability's own stat bonuses, the weapon's chance boosts at its fire rate,
// and its DPS with them, before the ability's effects.
struct Shooting {
  Character with_ability_gear;
  std::vector<ChanceBoost> boosts;
  Dps weapon;
};

Shooting ShootingOf(const Weapon& weapon, const Character& with_ability_gear, const Character& character,
                    const Target& target, const Assumptions& assumptions, const Breakdown& alone,
                    std::string_view stat) {
  Shooting out{with_ability_gear, {}, {alone.dps, 0}};
  double shots_per_second = 0;
  for (const SubattackResult& s : alone.subattacks) {
    shots_per_second = std::max(shots_per_second, s.volleys_per_second);
  }
  out.boosts = ChanceBoosts(weapon, character.worn, shots_per_second);
  if (!out.boosts.empty() || !stat.empty()) {
    out.weapon = MixedDps(weapon, with_ability_gear, target, assumptions, {}, out.boosts, stat);
  }
  return out;
}

// The ability sees the weapon's stat bonuses too, as in game, and its
// on-shoot stat changes, since the weapon is firing.
Character CasterOf(const Weapon& weapon, const Character& character) {
  return character.With(weapon.stat_bonuses).With(WhileShooting(weapon));
}

// The rate the ability is cast at (0 for as soon as possible) and the
// build's numbers at it. It's the rhythm that does the most for `weights`,
// never faster than MP and its cooldown allow: as soon as it can be, held
// back when an item's effect on casting would be blocked on one more cast,
// or less often when its own effects cost more damage than they add
// (Mirror Cloak's Weak). With equal weights, comparing the weighted DPS
// orders rates as the total does.
struct Rhythm {
  double rate = 0;
  BuildNumbers numbers;
};

Rhythm ChooseRhythm(const Weapon& weapon, const Ability& ability, const Character& caster, const Target& target,
                    const Assumptions& assumptions, const Shooting& shooting, const DpsWeights& weights) {
  const auto at = [&](const AbilityBreakdown& a) {
    const Dps cycle = CycleWeaponDps(a, shooting.weapon, weapon, shooting.with_ability_gear, target, assumptions,
                                     shooting.boosts, weights.stat);
    return BuildNumbers{cycle.total + a.dps, cycle.total, a.dps, cycle.stat + AbilityStatDps(a, weights.stat)};
  };
  const bool equal = weights.weapon == weights.ability;
  const auto worth = [&](const BuildNumbers& n) {
    if (!weights.stat.empty()) {
      return n.stat_dps + kTieBreak * n.dps;
    }
    return equal ? n.dps : weights.weapon * n.weapon_dps + weights.ability * n.ability_dps;
  };
  const AbilityBreakdown asap = EvaluateAbility(ability, caster, target, assumptions, 0, false);
  Rhythm out{0, at(asap)};
  for (const double rate : CastRates(asap)) {
    const BuildNumbers held = at(EvaluateAbility(ability, caster, target, assumptions, rate, false));
    if (worth(held) > worth(out.numbers) * (1 + 1e-12)) {
      out = {rate, held};
    }
  }
  return out;
}

}  // namespace

BuildBreakdown EvaluateBuild(const Weapon& weapon, const Ability* ability, const Character& character,
                             const Target& target, const Assumptions& assumptions, const DpsWeights& weights) {
  BuildBreakdown out;
  const Character with_ability_gear = ability ? character.With(ability->stat_bonuses) : character;
  out.weapon = EvaluateWeapon(weapon, with_ability_gear, target, assumptions);
  const Shooting shooting =
      ShootingOf(weapon, with_ability_gear, character, target, assumptions, out.weapon, weights.stat);
  for (const ChanceBoost& b : shooting.boosts) {
    out.uptimes.push_back({{Buff::On::kPlayer, b.bonus.stat, b.bonus.amount, b.duration_s}, b.share});
  }
  if (!shooting.boosts.empty()) {
    out.assumptions.push_back(
        "an on-shoot boost's cooldown starts when it goes off, and boosts go off independently of each other, with "
        "ones of the same stat counting once");
  }
  out.weapon_dps = shooting.weapon.total;
  out.stat_dps = shooting.weapon.stat;
  out.dps = out.weapon_dps;
  if (ability == nullptr) {
    return out;
  }
  // The explanation is written once, at the rhythm the numbers chose.
  const Character caster = CasterOf(weapon, character);
  const Rhythm rhythm = ChooseRhythm(weapon, *ability, caster, target, assumptions, shooting, weights);
  out.ability = EvaluateAbility(*ability, caster, target, assumptions, rhythm.rate);
  WithAbility(out, weapon, with_ability_gear, target, assumptions, shooting.boosts, weights.stat);
  return out;
}

BuildNumbers EvaluateBuildNumbers(const Weapon& weapon, const Ability* ability, const Character& character,
                                  const Target& target, const Assumptions& assumptions, const DpsWeights& weights) {
  const Character with_ability_gear = ability ? character.With(ability->stat_bonuses) : character;
  const Breakdown alone = EvaluateWeaponNumbers(weapon, with_ability_gear, target, assumptions);
  const Shooting shooting = ShootingOf(weapon, with_ability_gear, character, target, assumptions, alone, weights.stat);
  if (ability == nullptr) {
    return {shooting.weapon.total, shooting.weapon.total, 0, shooting.weapon.stat};
  }
  return ChooseRhythm(weapon, *ability, CasterOf(weapon, character), target, assumptions, shooting, weights).numbers;
}

namespace {

// Adds the ability's DPS and its buffs' effect on the weapon to `out`.
void WithAbility(BuildBreakdown& out, const Weapon& weapon, const Character& with_ability_gear, const Target& target,
                 const Assumptions& assumptions, const std::vector<ChanceBoost>& boosts, std::string_view stat) {
  const AbilityBreakdown& a = *out.ability;
  const double ability_stat = AbilityStatDps(a, stat);
  out.dps += a.dps;
  out.stat_dps += ability_stat;
  if (a.buffs.empty() || a.casts_per_second <= 0) {
    return;
  }
  const double period = 1 / a.casts_per_second;
  for (const Buff& b : a.buffs) {
    out.uptimes.push_back({b, std::clamp(b.duration_s, 0.0, period) / period});
  }
  const Dps cycle = CycleWeaponDps(a, {out.weapon_dps, out.stat_dps - ability_stat}, weapon, with_ability_gear,
                                   target, assumptions, boosts, stat);
  out.weapon_dps = cycle.total;
  out.stat_dps = cycle.stat + ability_stat;
  out.dps = out.weapon_dps + a.dps;
  out.assumptions.push_back(
      "every cast's buffs and debuffs start together and run their full duration, and a new cast doesn't extend "
      "one that's still up");
}

}  // namespace

namespace {

constexpr double kEpsilon = 1e-9;

// When the ability is cast in a window that opens with `mp`: as it opens,
// then as soon as the cooldown and MP allow, MP regenerating meanwhile.
std::vector<double> CastTimes(const AbilityBreakdown& a, double max_mp, double mp, double length) {
  const double cooldown = std::max(a.cooldown_s, 1e-3);
  const double regen = a.mp_per_second;
  std::vector<double> times;
  double t = 0;
  double ready = 0;
  while (true) {
    double at = ready;
    double mp_at = std::min(std::max(max_mp, mp), mp + regen * (at - t));
    if (mp_at + kEpsilon < a.mp_cost) {
      if (regen <= 0 || max_mp + kEpsilon < a.mp_cost) {
        break;
      }
      at = t + (a.mp_cost - mp) / regen;
      mp_at = a.mp_cost;
    }
    if (at > length + kEpsilon) {
      break;
    }
    times.push_back(at);
    t = at;
    mp = mp_at - a.mp_cost;
    ready = at + cooldown;
  }
  return times;
}

// The chance a part goes off on each cast of a window. One with a chance
// or a cooldown of its own rolls on each cast it's ready for, and once it
// goes off it can't again until its cooldown is over. It's ready when the
// window opens if its cooldown is no longer than the time between windows,
// and otherwise on that share of windows.
std::vector<double> TriggerOdds(double chance, double cooldown_s, std::span<const double> casts, double every_s) {
  if (chance >= 1 && cooldown_s <= 0) {
    return std::vector<double>(casts.size(), 1.0);
  }
  // When it's next ready, and how likely that is.
  std::map<double, double> states = {{-1e9, 1}};
  if (cooldown_s > every_s) {
    states = {{-1e9, every_s / cooldown_s}, {1e9, 1 - every_s / cooldown_s}};
  }
  std::vector<double> out;
  for (const double t : casts) {
    double fired = 0;
    std::map<double, double> next;
    for (const auto& [ready, odds] : states) {
      if (ready <= t + kEpsilon) {
        fired += odds * chance;
        next[t + cooldown_s] += odds * chance;
        next[ready] += odds * (1 - chance);
      } else {
        next[ready] += odds;
      }
    }
    out.push_back(fired);
    states = std::move(next);
  }
  return out;
}

// What the weapon does with one set of buffs up: each subattack's volleys
// per second and damage per volley, and each on-shoot effect's rate and
// damage. Chance boosts count as often as they're up on average.
struct Firing {
  std::vector<std::pair<double, double>> volleys;
  std::vector<std::pair<double, double>> procs;
  // The stat each proc's damage scales with.
  std::vector<std::string> proc_scaling;
};

Firing FiringWith(const Weapon& weapon, Character character, Target target, const Assumptions& assumptions,
                  const std::vector<const Buff*>& active, const std::vector<ChanceBoost>& boosts) {
  Apply(active, character, target);
  // Sums of rate and of damage per second over the mixes, which keep the
  // weapon's average DPS.
  Firing out;
  for (const auto& [weight, c] : BoostMixes(weapon, character, boosts)) {
    const Breakdown b = EvaluateWeaponNumbers(weapon, c, target, assumptions);
    out.volleys.resize(b.subattacks.size());
    out.procs.resize(b.procs.size());
    out.proc_scaling.resize(b.procs.size());
    for (std::size_t i = 0; i < b.procs.size(); ++i) out.proc_scaling[i] = b.procs[i].scaling;
    for (std::size_t i = 0; i < b.subattacks.size(); ++i) {
      out.volleys[i].first += weight * b.subattacks[i].volleys_per_second;
      out.volleys[i].second += weight * b.subattacks[i].dps;
    }
    for (std::size_t i = 0; i < b.procs.size(); ++i) {
      out.procs[i].first += weight * b.procs[i].per_second;
      out.procs[i].second += weight * b.procs[i].dps;
    }
  }
  for (auto* list : {&out.volleys, &out.procs}) {
    for (auto& [rate, damage] : *list) {
      damage = rate > 0 ? damage / rate : 0;
    }
  }
  return out;
}

}  // namespace

BurstBreakdown EvaluateBurst(const Weapon& weapon, const Ability* ability, const Character& character,
                             const Target& target, const Assumptions& assumptions, const BurstWindow& window,
                             std::string_view stat) {
  BurstBreakdown out;
  const double length = std::max(0.0, window.length_s);

  // The most casts a window can hold is what a full pool of MP allows. In
  // the long run, though, each window can only spend what regenerates in
  // one period, so a window may average a share of a cast: then the
  // windows with one more cast and one fewer are mixed.
  std::optional<AbilityBreakdown> a;
  std::vector<double> casts;
  double average = 0;
  if (ability != nullptr) {
    const Character caster = character.With(weapon.stat_bonuses).With(WhileShooting(weapon));
    a = EvaluateAbility(*ability, caster, target, assumptions);
    const double max_mp = caster.With(ability->stat_bonuses).max_magic_points;
    casts = CastTimes(*a, max_mp, max_mp, length);
    average = static_cast<double>(casts.size());
    out.window_casts = static_cast<int>(casts.size());
    if (a->mp_cost > 0) {
      // MP given back on the window's casts pays toward the next window's,
      // and how much comes back depends on how many casts there are.
      // MP back after the first k casts of a window, and for an average
      // number of casts, between the whole numbers around it.
      std::vector<double> back_after(casts.size() + 1, 0);
      for (const MpRefund& r : a->refunds) {
        const std::vector<double> odds = TriggerOdds(r.chance, r.cooldown_s, casts, window.every_s);
        for (std::size_t k = 0; k < odds.size(); ++k) {
          back_after[k + 1] += odds[k] * r.amount;
        }
      }
      for (std::size_t k = 1; k < back_after.size(); ++k) {
        back_after[k] += back_after[k - 1];
      }
      const auto back = [&](double n) {
        const double whole = std::clamp(std::floor(n), 0.0, static_cast<double>(casts.size()));
        const auto i = static_cast<std::size_t>(whole);
        const double next = i + 1 < back_after.size() ? back_after[i + 1] : back_after[i];
        return back_after[i] + (n - whole) * (next - back_after[i]);
      };
      out.paid_casts = a->mp_per_second * window.every_s / a->mp_cost;
      for (int settle = 0; settle < 50 && !a->refunds.empty(); ++settle) {
        out.paid_casts = (a->mp_per_second * window.every_s + back(out.paid_casts)) / a->mp_cost;
      }
      average = std::min(average, out.paid_casts);
    }
    out.assumptions.push_back(
        "the ability is cast as the window opens, its buffs up for the first volley, and again as soon as its "
        "cooldown and MP allow; over many windows, each can only spend the MP that regenerates in between and what "
        "the gear gives back on casting");
    out.assumptions.push_back("a new cast restarts the buffs and debuffs of the last");
  }
  out.casts = average;
  const std::vector<Buff> buffs = a ? a->buffs : std::vector<Buff>{};

  const Character shooter = ability ? character.With(ability->stat_bonuses) : character;
  double shots_per_second = 0;
  for (const SubattackResult& s : EvaluateWeaponNumbers(weapon, shooter, target, assumptions).subattacks) {
    shots_per_second = std::max(shots_per_second, s.volleys_per_second);
  }
  const std::vector<ChanceBoost> boosts = ChanceBoosts(weapon, character.worn, shots_per_second);
  std::map<unsigned, Firing> known;
  const auto firing = [&](unsigned mask) -> const Firing& {
    auto it = known.find(mask);
    if (it == known.end()) {
      std::vector<const Buff*> active;
      for (std::size_t i = 0; i < buffs.size(); ++i) {
        if ((mask >> i) & 1u) {
          active.push_back(&buffs[i]);
        }
      }
      it = known.emplace(mask, FiringWith(weapon, shooter, target, assumptions, active, boosts)).first;
    }
    return it->second;
  };

  // One window with the first `count` casts: the weapon's and the
  // ability's damage. Each subattack and each on-shoot effect fires from the
  // window's start at the rate whatever is up then allows.
  const auto damage_with = [&](std::size_t count) {
    const auto up_at = [&](double t) {
      unsigned mask = 0;
      for (std::size_t i = 0; i < buffs.size(); ++i) {
        for (std::size_t k = 0; k < count; ++k) {
          if (casts[k] <= t + kEpsilon && t < casts[k] + buffs[i].duration_s - kEpsilon) {
            mask |= 1u << i;
          }
        }
      }
      return mask;
    };
    // The weapon's damage, the ability's, and the part of both that scales
    // with `stat`.
    std::array<double, 3> sum{};
    const std::size_t subattacks = firing(0).volleys.size();
    const std::size_t procs = firing(0).procs.size();
    for (std::size_t i = 0; i < subattacks + procs; ++i) {
      const bool counts = !stat.empty() && (i < subattacks ? ShotsScaleWith(stat)
                                                           : ProcScalesWith(firing(0).proc_scaling[i - subattacks], stat));
      for (double t = 0; t <= length + kEpsilon;) {
        const Firing& f = firing(up_at(t));
        const auto& [per_second, damage] = i < subattacks ? f.volleys[i] : f.procs[i - subattacks];
        sum[0] += damage;
        if (counts) sum[2] += damage;
        if (per_second <= 0) {
          break;
        }
        t += 1 / per_second;
      }
    }
    for (const AbilityPart& p : a ? a->parts : std::vector<AbilityPart>{}) {
      const std::vector<double> odds = TriggerOdds(p.chance, p.cooldown_s, std::span(casts).first(count), window.every_s);
      for (std::size_t k = 0; k < count; ++k) {
        const double landed = p.over_s > 0 ? std::clamp((length - casts[k]) / p.over_s, 0.0, 1.0) : 1.0;
        sum[1] += odds[k] * landed * p.damage / p.share;
        if (!stat.empty() && p.scaling == stat) sum[2] += odds[k] * landed * p.damage / p.share;
      }
    }
    return sum;
  };
  const auto whole = static_cast<std::size_t>(std::floor(average + kEpsilon));
  const double extra = whole < casts.size() ? average - static_cast<double>(whole) : 0;
  const auto [weapon_damage, ability_damage, stat_damage] = damage_with(whole);
  out.weapon_damage = weapon_damage;
  out.ability_damage = ability_damage;
  out.stat_damage = stat_damage;
  if (extra > kEpsilon) {
    const auto [more_weapon, more_ability, more_stat] = damage_with(whole + 1);
    out.weapon_damage += extra * (more_weapon - weapon_damage);
    out.ability_damage += extra * (more_ability - ability_damage);
    out.stat_damage += extra * (more_stat - stat_damage);
  }
  out.assumptions.push_back(
      "you fire from the moment the window opens, and a volley fired in it counts in full; on-shoot effects are "
      "ready when it opens");
  if (!boosts.empty()) {
    out.assumptions.push_back("on-shoot chance boosts count as often as they're up on average");
  }
  out.damage = out.weapon_damage + out.ability_damage;
  return out;
}

}  // namespace rotmg
