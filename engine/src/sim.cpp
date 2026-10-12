#include "rotmg/sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <utility>

#include "rotmg/damage.hpp"
#include "rotmg/evaluate.hpp"
#include "rotmg/fire_rate.hpp"

namespace rotmg {
namespace {

Scenario OneTarget(const Target& target, double distance, double radius) {
  return {{0, 0}, {{{0, distance}, radius, target}}, 0};
}

// How far along the segment from a to b, from 0 to 1, a shot first touches a
// circle, or nothing if it doesn't. A segment of zero length is a point.
std::optional<double> Entry(Point a, Point b, Point centre, double radius) {
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double length2 = dx * dx + dy * dy;
  const double t = length2 > 0 ? std::clamp(((centre.x - a.x) * dx + (centre.y - a.y) * dy) / length2, 0.0, 1.0) : 0;
  const double ex = a.x + t * dx - centre.x;
  const double ey = a.y + t * dy - centre.y;
  const double miss2 = ex * ex + ey * ey;
  if (miss2 > radius * radius) {
    return std::nullopt;
  }
  if (length2 == 0) {
    return 0.0;
  }
  // Back from the closest point to where the segment crosses the circle.
  return std::max(0.0, t - std::sqrt((radius * radius - miss2) / length2));
}

}  // namespace

Simulation::Simulation(const Weapon& weapon, const Character& character, const Target& target, double distance,
                       double radius)
    : Simulation(weapon, character, OneTarget(target, distance, radius)) {}

Simulation::Simulation(const Weapon& weapon, const Character& character, const Scenario& scenario)
    : weapon_(weapon),
      world_(scenario),
      per_subattack_(weapon.subattacks.size()),
      per_target_(scenario.targets.size()) {
  const Character geared = character.With(weapon_.stat_bonuses).With(WhileShooting(weapon_));
  const int attack = geared.attack;
  const int dexterity = geared.dexterity;
  const Multiplier multiplier = WithDamageBonus(DamageMultiplier(attack, character.status), weapon_.damage_bonus);
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
    std::vector<double> damage;
    for (const SimTarget& t : world_.targets) {
      damage.push_back(p ? ExpectedHitDamage(*p, multiplier, t.target) : 0);
    }
    damage_per_hit_.push_back(std::move(damage));
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

Point Simulation::TargetCenter() const {
  return world_.aim < world_.targets.size() ? world_.targets[world_.aim].position : Point{};
}

double Simulation::Radius() const {
  return world_.aim < world_.targets.size() ? world_.targets[world_.aim].radius : 0;
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
    const bool hit = std::ranges::find(l.hit, true) != l.hit.end();
    shots_.push_back({l.subattack, weapon_.subattacks[static_cast<std::size_t>(l.subattack)].projectile_id,
                      WorldAt(l, l.step), path.HeadingDegrees(l.step) + l.aim * 180 / std::numbers::pi,
                      l.step * kStepMs, hit});
  }
}

Point Simulation::WorldAt(const Live& shot, int step) const {
  const ProjectilePath& path = paths_[static_cast<std::size_t>(shot.subattack)][static_cast<std::size_t>(shot.index)];
  const Point local = path.At(step);
  // The path's +y is the aim and its +x the aim's right.
  const double s = std::sin(shot.aim);
  const double c = std::cos(shot.aim);
  return {shot.origin.x + local.x * c + local.y * s, shot.origin.y - local.x * s + local.y * c};
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
    if (l.stopped || l.step >= path.Steps()) {
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
  const Point from = world_.player;
  const Point to = TargetCenter();
  const double aim = to.x == from.x && to.y == from.y ? 0 : std::atan2(to.x - from.x, to.y - from.y);
  for (int i = 0; i < static_cast<int>(paths.size()); ++i) {
    ++totals_.projectiles;
    ++counters.projectiles;
    live_.push_back({subattack, i, id, 0, from, aim, std::vector<bool>(world_.targets.size(), false), false});
    Land(live_.back());
  }
  // A volley with no projectiles is finished as soon as it's fired.
  ResolveFinished();
}

void Simulation::Land(Live& shot) {
  if (shot.stopped) {
    return;
  }
  const ProjectilePath& path = paths_[static_cast<std::size_t>(shot.subattack)][static_cast<std::size_t>(shot.index)];
  if (path.Steps() == 0) {
    return;
  }
  const Point here = WorldAt(shot, shot.step);
  const Point before = shot.step == 0 ? here : WorldAt(shot, shot.step - 1);
  const bool pierces = pierces_[static_cast<std::size_t>(shot.subattack)];

  // A shot that doesn't pierce stops at the first target it reaches this
  // step; one that pierces hits every target it passes through.
  std::vector<std::pair<double, std::size_t>> touched;
  for (std::size_t i = 0; i < world_.targets.size(); ++i) {
    if (shot.hit[i]) {
      continue;
    }
    const SimTarget& t = world_.targets[i];
    if (const auto entry = Entry(before, here, t.position, t.radius)) {
      touched.push_back({*entry, i});
    }
  }
  if (touched.empty()) {
    return;
  }
  std::ranges::sort(touched);
  if (!pierces) {
    touched.resize(1);
    shot.stopped = true;
  }

  for (const auto& [entry, i] : touched) {
    shot.hit[i] = true;
    const double damage = damage_per_hit_[static_cast<std::size_t>(shot.subattack)][i];
    Counters& counters = per_subattack_[static_cast<std::size_t>(shot.subattack)];
    ++totals_.hits;
    ++counters.hits;
    totals_.damage += damage;
    counters.damage += damage;
    ++per_target_[i].hits;
    per_target_[i].damage += damage;
    for (OpenVolley& v : open_) {
      if (v.id == shot.volley) {
        ++v.hits;
        break;
      }
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
