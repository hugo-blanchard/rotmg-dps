#include "rotmg/sim.hpp"

#include <algorithm>
#include <cstddef>

#include "rotmg/damage.hpp"
#include "rotmg/evaluate.hpp"
#include "rotmg/fire_rate.hpp"

namespace rotmg {

Simulation::Simulation(const Weapon& weapon, const Character& character, const Target& target,
                       double distance, double radius)
    : weapon_(weapon), distance_(distance), radius_(radius), per_subattack_(weapon.subattacks.size()) {
  const Character geared = character.With(weapon_.stat_bonuses).With(WhileShooting(weapon_));
  const int attack = geared.attack;
  const int dexterity = geared.dexterity;
  const Multiplier multiplier = DamageMultiplier(attack, character.status);
  const double attacks_per_second = AttacksPerSecond(dexterity, character.status);

  for (const Subattack& s : weapon_.subattacks) {
    Clock clock;
    clock.interval_ms = 1000 / (attacks_per_second * s.rate_of_fire);
    if (s.burst_count && s.burst_delay) {
      clock.burst = true;
      clock.burst_count = *s.burst_count;
      clock.burst_delay_ms = BurstDelay(s, dexterity, character.status) * 1000;
    }
    clocks_.push_back(clock);

    const Projectile* p = weapon_.FindProjectile(s.projectile_id);
    damage_per_hit_.push_back(p ? ExpectedHitDamage(*p, multiplier, target) : 0);
    pierces_.push_back(p && p->multi_hit);
    std::vector<ProjectilePath> paths;
    if (p) {
      for (int i = 0; i < s.num_projectiles; ++i) {
        paths.emplace_back(*p, s, i);
      }
    }
    paths_.push_back(std::move(paths));
  }
}

void Simulation::Advance(double ms) {
  carry_ms_ += ms;
  while (carry_ms_ >= kStepMs) {
    carry_ms_ -= kStepMs;
    Step();
  }
  shots_.clear();
  for (const Live& l : live_) {
    const ProjectilePath& path = paths_[static_cast<std::size_t>(l.subattack)][static_cast<std::size_t>(l.index)];
    shots_.push_back({l.subattack, weapon_.subattacks[static_cast<std::size_t>(l.subattack)].projectile_id,
                      path.At(l.step), path.HeadingDegrees(l.step), l.step * kStepMs, l.hit});
  }
}

void Simulation::Step() {
  ++steps_;
  const double now = TimeMs();

  // Shots already in flight move first, then new volleys spawn, so a shot
  // fired this step is checked at its spawn point and nothing else.
  for (Live& l : live_) {
    ++l.step;
    Land(l);
  }
  for (std::size_t i = 0; i < clocks_.size(); ++i) {
    Clock& c = clocks_[i];
    while (c.next_ms <= now) {
      Fire(static_cast<int>(i));
      if (!c.burst) {
        c.next_ms += c.interval_ms;
        continue;
      }
      if (c.burst_fired == 0) {
        c.burst_start_ms = c.next_ms;
      }
      if (++c.burst_fired < c.burst_count) {
        c.next_ms += c.interval_ms;
      } else {
        // The delay runs from the start of one burst to the start of the next.
        c.burst_fired = 0;
        c.next_ms = std::max(c.next_ms + c.interval_ms, c.burst_start_ms + c.burst_delay_ms);
      }
    }
  }

  std::erase_if(live_, [this](const Live& l) {
    const ProjectilePath& path = paths_[static_cast<std::size_t>(l.subattack)][static_cast<std::size_t>(l.index)];
    const bool stopped = l.hit && !pierces_[static_cast<std::size_t>(l.subattack)];
    if (stopped || l.step >= path.Steps()) {
      Retire(l);
      return true;
    }
    return false;
  });
}

void Simulation::Fire(int subattack) {
  const auto& paths = paths_[static_cast<std::size_t>(subattack)];
  Counters& counters = per_subattack_[static_cast<std::size_t>(subattack)];
  ++totals_.volleys;
  ++counters.volleys;
  const long id = next_volley_++;
  open_.push_back({id, subattack, static_cast<int>(paths.size()), 0});
  for (int i = 0; i < static_cast<int>(paths.size()); ++i) {
    ++totals_.projectiles;
    ++counters.projectiles;
    live_.push_back({subattack, i, id, 0, false});
    Land(live_.back());
  }
  // A volley with no projectiles is finished as soon as it's fired.
  ResolveFinished();
}

void Simulation::Land(Live& shot) {
  if (shot.hit) {
    return;
  }
  const ProjectilePath& path = paths_[static_cast<std::size_t>(shot.subattack)][static_cast<std::size_t>(shot.index)];
  if (!path.Touches(shot.step, TargetCenter(), radius_)) {
    return;
  }
  shot.hit = true;
  const double damage = damage_per_hit_[static_cast<std::size_t>(shot.subattack)];
  Counters& counters = per_subattack_[static_cast<std::size_t>(shot.subattack)];
  ++totals_.hits;
  ++counters.hits;
  totals_.damage += damage;
  counters.damage += damage;
  for (OpenVolley& v : open_) {
    if (v.id == shot.volley) {
      ++v.hits;
      break;
    }
  }
}

void Simulation::Retire(const Live& shot) {
  for (OpenVolley& v : open_) {
    if (v.id == shot.volley) {
      --v.remaining;
      break;
    }
  }
  ResolveFinished();
}

void Simulation::ResolveFinished() {
  // Volleys resolve in the order they were fired, so the counters only ever
  // include volleys that are completely finished.
  while (!open_.empty() && open_.front().remaining == 0) {
    const OpenVolley& v = open_.front();
    Counters& counters = per_subattack_[static_cast<std::size_t>(v.subattack)];
    ++totals_.resolved_volleys;
    ++counters.resolved_volleys;
    totals_.resolved_hits += v.hits;
    counters.resolved_hits += v.hits;
    open_.pop_front();
  }
}

}  // namespace rotmg
