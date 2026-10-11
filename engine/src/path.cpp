#include "rotmg/path.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rotmg {
namespace {

// Wavy shots: fixed frequency and the amplitude they grow to (wiki).
constexpr double kWavyCycles = 2.75;
constexpr double kWavyPeak = 0.4;
// Arc gap when a multi-shot subattack doesn't set one (wiki).
constexpr double kDefaultArcGap = 11.25;

double DistanceToSegment(Point p, Point a, Point b) {
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

ProjectilePath::ProjectilePath(const Projectile& projectile, const Subattack& subattack, int index)
    : projectile_(&projectile) {
  if (projectile.lifetime_ms <= 0) {
    return;
  }
  steps_ = std::max(1, static_cast<int>(std::ceil(projectile.lifetime_ms / kStepMs)));
  forward_ = ForwardDistances(projectile, steps_);
  const int n = subattack.num_projectiles;
  degrees_ = subattack.default_angle.value_or(0) + (index - (n - 1) / 2.0) * subattack.arc_gap.value_or(kDefaultArcGap);
  const double angle = degrees_ * std::numbers::pi / 180;
  heading_ = {std::sin(angle), std::cos(angle)};
  side_ = {std::cos(angle), -std::sin(angle)};
  const auto [x, y] = subattack.pos_offset.value_or(std::array<double, 2>{0, 0});
  offset_ = {x, y};
  polarity_ = index % 2 == 0 ? 1 : -1;
}

Point ProjectilePath::At(int step) const {
  if (steps_ == 0) {
    return offset_;
  }
  step = std::clamp(step, 0, steps_);
  // Boomerang shots fly out for half their life and come back the same way.
  const int s = projectile_->boomerang && step > steps_ / 2 ? steps_ - step : step;
  const double along = forward_[static_cast<std::size_t>(s)];
  const double across = Sideways(*projectile_, step * kStepMs, polarity_);
  return {offset_.x + along * heading_.x + across * side_.x, offset_.y + along * heading_.y + across * side_.y};
}

bool ProjectilePath::Touches(int step, Point target, double radius) const {
  if (steps_ == 0 || step < 0 || step > steps_) {
    return false;
  }
  const Point here = At(step);
  if (step == 0) {
    return std::hypot(here.x - target.x, here.y - target.y) <= radius;
  }
  return DistanceToSegment(target, At(step - 1), here) <= radius;
}

}  // namespace rotmg
