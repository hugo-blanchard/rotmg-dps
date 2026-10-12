#include "rotmg/build.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rotmg {
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

// The weapon's DPS with a set of buffs and debuffs up.
double WeaponDps(const Weapon& weapon, Character character, Target target, const Assumptions& assumptions,
                 const std::vector<const Buff*>& active) {
  Apply(active, character, target);
  return EvaluateWeaponDps(weapon, character, target, assumptions);
}

// The mixes of the weapon's chance boosts, each up or not on its own, as
// the character each makes and how often it comes up. Boosts of stats the
// weapon doesn't read leave it as it is, so mixes that differ only in those
// come back as one: ATT and DEX count, and any stat an on-shoot effect
// scales with.
std::vector<std::pair<double, Character>> BoostMixes(const Weapon& weapon, const Character& character,
                                                     const std::vector<ChanceBoost>& boosts) {
  std::vector<std::string> read = {"ATT", "DEX"};
  for (const Activate& a : weapon.on_shoot) {
    if (const std::string stat = a.Text("scalingStat"); !stat.empty() && std::ranges::find(read, stat) == read.end()) {
      read.push_back(stat);
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
double MixedDps(const Weapon& weapon, const Character& character, const Target& target,
                const Assumptions& assumptions, const std::vector<const Buff*>& active,
                const std::vector<ChanceBoost>& boosts) {
  double total = 0;
  for (const auto& [weight, c] : BoostMixes(weapon, character, boosts)) {
    total += weight * WeaponDps(weapon, c, target, assumptions, active);
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
std::vector<double> TriggerOdds(const AbilityPart& p, std::span<const double> casts, double every_s) {
  if (p.chance >= 1 && p.cooldown_s <= 0) {
    return std::vector<double>(casts.size(), 1.0);
  }
  // When it's next ready, and how likely that is.
  std::map<double, double> states = {{-1e9, 1}};
  if (p.cooldown_s > every_s) {
    states = {{-1e9, every_s / p.cooldown_s}, {1e9, 1 - every_s / p.cooldown_s}};
  }
  std::vector<double> out;
  for (const double t : casts) {
    double fired = 0;
    std::map<double, double> next;
    for (const auto& [ready, odds] : states) {
      if (ready <= t + kEpsilon) {
        fired += odds * p.chance;
        next[t + p.cooldown_s] += odds * p.chance;
        next[ready] += odds * (1 - p.chance);
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
                             const Target& target, const Assumptions& assumptions, const BurstWindow& window) {
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
      out.paid_casts = a->mp_per_second * window.every_s / a->mp_cost;
      average = std::min(average, out.paid_casts);
    }
    out.assumptions.push_back(
        "the ability is cast as the window opens, its buffs up for the first volley, and again as soon as its "
        "cooldown and MP allow; over many windows, each can only spend the MP that regenerates in between");
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
    std::pair<double, double> sum{0, 0};
    const std::size_t subattacks = firing(0).volleys.size();
    const std::size_t procs = firing(0).procs.size();
    for (std::size_t i = 0; i < subattacks + procs; ++i) {
      for (double t = 0; t <= length + kEpsilon;) {
        const Firing& f = firing(up_at(t));
        const auto& [per_second, damage] = i < subattacks ? f.volleys[i] : f.procs[i - subattacks];
        sum.first += damage;
        if (per_second <= 0) {
          break;
        }
        t += 1 / per_second;
      }
    }
    for (const AbilityPart& p : a ? a->parts : std::vector<AbilityPart>{}) {
      const std::vector<double> odds = TriggerOdds(p, std::span(casts).first(count), window.every_s);
      for (std::size_t k = 0; k < count; ++k) {
        const double landed = p.over_s > 0 ? std::clamp((length - casts[k]) / p.over_s, 0.0, 1.0) : 1.0;
        sum.second += odds[k] * landed * p.damage / p.share;
      }
    }
    return sum;
  };
  const auto whole = static_cast<std::size_t>(std::floor(average + kEpsilon));
  const double extra = whole < casts.size() ? average - static_cast<double>(whole) : 0;
  const auto [weapon_damage, ability_damage] = damage_with(whole);
  out.weapon_damage = weapon_damage;
  out.ability_damage = ability_damage;
  if (extra > kEpsilon) {
    const auto [more_weapon, more_ability] = damage_with(whole + 1);
    out.weapon_damage += extra * (more_weapon - weapon_damage);
    out.ability_damage += extra * (more_ability - ability_damage);
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
