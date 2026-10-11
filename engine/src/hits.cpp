#include "rotmg/hits.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace rotmg {
namespace {

// Small enough that the fastest projectiles move about 0.05 tiles a step.
// Hits are found on the segment between steps, so nothing slips through.
constexpr double kStepMs = 2.0;
// Wavy shots: fixed frequency and the amplitude they grow to (wiki).
constexpr double kWavyCycles = 2.75;
constexpr double kWavyPeak = 0.4;
// Arc gap when a multi-shot subattack doesn't set one (wiki).
constexpr double kDefaultArcGap = 11.25;

// Unmodelled client tags that change where projectiles go.
const std::set<std::string, std::less<>> kMotionTags = {
    "TurnRate",        "TurnRateDelay",   "TurnStopTime",      "TurnAcceleration", "TurnAccelerationDelay",
    "TurnClamp",       "CircleTurnAngle", "CircleTurnDelay",   "CollisionMult",    "ProjectilePattern",
    "DefaultAngleIncr",
};

struct Vec {
  double x = 0;
  double y = 0;
};

double DistanceToSegment(Vec p, Vec a, Vec b) {
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double length2 = dx * dx + dy * dy;
  double t = length2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length2 : 0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

// Distance travelled along the heading at each step. Speed and acceleration
// are in the client's tenths of a tile per second (per second); the
// Torrential Staff's -120 is the wiki's "-12 t/s^2 after 0.1 s".
std::vector<double> ForwardDistances(const Projectile& p, int steps) {
  std::vector<double> out(static_cast<std::size_t>(steps) + 1, 0.0);
  double speed = p.speed / 10;
  const double accel = p.acceleration.value_or(0) / 10;
  const double delay = p.acceleration_delay.value_or(0);
  const double dt = kStepMs / 1000;
  for (int i = 1; i <= steps; ++i) {
    if (accel != 0 && (i - 1) * kStepMs >= delay) {
      speed += accel * dt;
      if (p.speed_clamp) {
        const double clamp = *p.speed_clamp / 10;
        speed = accel < 0 ? std::max(speed, clamp) : std::min(speed, clamp);
      }
    }
    out[static_cast<std::size_t>(i)] = out[static_cast<std::size_t>(i) - 1] + speed * dt;
  }
  return out;
}

// Sideways offset from the heading at time t. `polarity` flips the wave for
// every other projectile in a volley, as with a tiered staff's two shots.
double Sideways(const Projectile& p, double t_ms, double polarity) {
  if (p.lifetime_ms <= 0) {
    return 0;
  }
  const double phase = t_ms / p.lifetime_ms;
  if (p.wavy) {
    return polarity * kWavyPeak * phase * std::sin(2 * std::numbers::pi * kWavyCycles * phase);
  }
  const double amplitude = p.amplitude.value_or(0);
  if (amplitude == 0) {
    return 0;
  }
  return polarity * amplitude * std::sin(2 * std::numbers::pi * p.frequency.value_or(1) * phase);
}

}  // namespace

std::vector<std::string> HitModelGaps(const Weapon& weapon) {
  std::vector<std::string> gaps;
  for (const Projectile& p : weapon.projectiles) {
    if (p.parametric) {
      gaps.emplace_back("figure-eight (parametric) shots");
      break;
    }
  }
  for (const std::string& tag : weapon.unsupported) {
    if (kMotionTags.contains(tag)) {
      gaps.emplace_back("projectiles that turn, circle or change pattern");
      break;
    }
  }
  return gaps;
}

int HitsPerVolley(const Weapon& weapon, const Subattack& subattack, double distance, double radius) {
  const Projectile* p = weapon.FindProjectile(subattack.projectile_id);
  // Placeholder projectiles have no lifetime; they never exist long enough
  // to hit anything, even a target right next to the player.
  if (p == nullptr || subattack.num_projectiles <= 0 || p->lifetime_ms <= 0) {
    return 0;
  }
  const int steps = std::max(1, static_cast<int>(std::ceil(p->lifetime_ms / kStepMs)));
  const std::vector<double> forward = ForwardDistances(*p, steps);
  const Vec target{0, distance};
  const double arc_gap = subattack.arc_gap.value_or(kDefaultArcGap);
  // The player aims along +y. A positive angle turns toward +x, and PosOffset
  // is (sideways, forward) in the same frame: that's the reading under which
  // the Hama Yumi's and Torrential Staff's offset shots converge, as the wiki
  // says they do.
  const auto [offset_x, offset_y] = subattack.pos_offset.value_or(std::array<double, 2>{0, 0});

  int hits = 0;
  const int n = subattack.num_projectiles;
  for (int i = 0; i < n; ++i) {
    const double degrees = subattack.default_angle.value_or(0) + (i - (n - 1) / 2.0) * arc_gap;
    const double angle = degrees * std::numbers::pi / 180;
    const Vec heading{std::sin(angle), std::cos(angle)};
    const Vec side{std::cos(angle), -std::sin(angle)};
    const double polarity = i % 2 == 0 ? 1 : -1;

    auto at = [&](int step) {
      // Boomerang shots fly out for half their life and come back the same way.
      int s = step;
      if (p->boomerang && step > steps / 2) {
        s = steps - step;
      }
      const double along = forward[static_cast<std::size_t>(s)];
      const double across = Sideways(*p, step * kStepMs, polarity);
      return Vec{offset_x + along * heading.x + across * side.x, offset_y + along * heading.y + across * side.y};
    };

    Vec previous = at(0);
    bool hit = std::hypot(previous.x - target.x, previous.y - target.y) <= radius;
    for (int step = 1; step <= steps && !hit; ++step) {
      const Vec current = at(step);
      hit = DistanceToSegment(target, previous, current) <= radius;
      previous = current;
    }
    hits += hit ? 1 : 0;
  }
  return hits;
}

std::vector<std::string> HitModelAssumptions(const Weapon& weapon) {
  std::vector<std::string> out;
  bool wavy = false;
  bool default_frequency = false;
  for (const Projectile& p : weapon.projectiles) {
    wavy = wavy || p.wavy;
    default_frequency = default_frequency || (p.amplitude.value_or(0) != 0 && !p.frequency);
  }
  const bool offset = std::ranges::any_of(weapon.subattacks, [](const Subattack& s) { return s.pos_offset.has_value(); });
  if (wavy) {
    out.emplace_back("wavy shots' amplitude grows evenly to 0.4 tiles");
  }
  if (default_frequency) {
    out.emplace_back("a sine wave with no frequency set makes 1 cycle");
  }
  if (offset) {
    out.emplace_back("PosOffset is sideways then forward, in tiles");
  }
  return out;
}

}  // namespace rotmg
