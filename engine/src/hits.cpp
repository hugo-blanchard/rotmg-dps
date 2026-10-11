#include "rotmg/hits.hpp"

#include <algorithm>
#include <set>

#include "rotmg/path.hpp"

namespace rotmg {
namespace {

// Unmodelled client tags that change where projectiles go.
const std::set<std::string, std::less<>> kMotionTags = {
    "TurnRate",        "TurnRateDelay",   "TurnStopTime",      "TurnAcceleration", "TurnAccelerationDelay",
    "TurnClamp",       "CircleTurnAngle", "CircleTurnDelay",   "CollisionMult",    "ProjectilePattern",
    "DefaultAngleIncr",
};

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
  if (p == nullptr) {
    return 0;
  }
  const Point target{0, distance};
  int hits = 0;
  for (int i = 0; i < subattack.num_projectiles; ++i) {
    const ProjectilePath path(*p, subattack, i);
    for (int step = 0; step <= path.Steps(); ++step) {
      if (path.Touches(step, target, radius)) {
        ++hits;
        break;
      }
    }
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
