#include "rotmg/hits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include "rotmg/path.hpp"

namespace rotmg {
namespace {

// Unmodelled client tags that change where projectiles go. The CircleTurn
// tags are modelled now, but datasets made before circleTurnDelay and
// circleTurnAngle existed still flag them as unsupported.
const std::set<std::string, std::less<>> kMotionTags = {
    "TurnRate",        "TurnRateDelay",   "TurnStopTime",      "TurnAcceleration", "TurnAccelerationDelay",
    "TurnClamp",       "CircleTurnAngle", "CircleTurnDelay",   "CollisionMult",    "ProjectilePattern",
    "DefaultAngleIncr",
};

// Everything a volley's paths depend on (see path.cpp), for remembering
// estimates. A missing value is NaN, which no real value is.
using Key = std::array<double, 18>;

Key VolleyKey(const Projectile& p, const Subattack& s, double distance, double radius) {
  constexpr double kNone = std::numeric_limits<double>::quiet_NaN();
  const auto [x, y] = s.pos_offset.value_or(std::array<double, 2>{kNone, kNone});
  return {p.speed,
          p.lifetime_ms,
          p.acceleration.value_or(kNone),
          p.acceleration_delay.value_or(kNone),
          p.speed_clamp.value_or(kNone),
          p.amplitude.value_or(kNone),
          p.frequency.value_or(kNone),
          p.wavy ? 1.0 : 0.0,
          p.boomerang ? 1.0 : 0.0,
          p.circle_turn_delay.value_or(kNone),
          p.circle_turn_angle.value_or(kNone),
          static_cast<double>(s.num_projectiles),
          s.default_angle.value_or(kNone),
          s.arc_gap.value_or(kNone),
          x,
          y,
          distance,
          radius};
}

// Orders keys with NaN equal to NaN, unlike operator<.
struct KeyLess {
  bool operator()(const Key& a, const Key& b) const {
    return std::ranges::lexicographical_compare(a, b, [](double x, double y) {
      if (std::isnan(x) || std::isnan(y)) {
        return std::isnan(y) && !std::isnan(x);
      }
      return x < y;
    });
  }
};

int TraceHits(const Projectile& p, const Subattack& subattack, double distance, double radius) {
  const Point target{0, distance};
  int hits = 0;
  for (int i = 0; i < subattack.num_projectiles; ++i) {
    const ProjectilePath path(p, subattack, i);
    for (int step = 0; step <= path.Steps(); ++step) {
      if (path.Touches(step, target, radius)) {
        ++hits;
        break;
      }
    }
  }
  return hits;
}

}  // namespace

bool HitModelFits(const Weapon& weapon) {
  return std::ranges::none_of(weapon.projectiles, &Projectile::parametric) &&
         std::ranges::none_of(weapon.unsupported, [](const std::string& tag) { return kMotionTags.contains(tag); });
}

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
  // Tracing long-lived or circling shots is slow, and the optimizer asks
  // about the same volleys over and over, so estimates are remembered. The
  // answer depends only on the key, so this can't change a result.
  thread_local std::map<Key, int, KeyLess> known;
  if (known.size() > 4096) {
    known.clear();
  }
  const Key key = VolleyKey(*p, subattack, distance, radius);
  if (const auto it = known.find(key); it != known.end()) {
    return it->second;
  }
  const int hits = TraceHits(*p, subattack, distance, radius);
  known.emplace(key, hits);
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

std::vector<double> DistancesWorthScoring(const Weapon& weapon, const Ability* ability,
                                          const std::vector<double>& distances, double radius) {
  std::vector<const Weapon*> shooters;
  if (HitModelFits(weapon)) {
    shooters.push_back(&weapon);
  }
  Weapon ability_shots;
  if (ability != nullptr && !ability->subattacks.empty()) {
    ability_shots = ability->AsWeapon();
    if (HitModelFits(ability_shots)) {
      shooters.push_back(&ability_shots);
    }
  }
  const auto hits_at = [&](double distance) {
    std::vector<int> out;
    for (const Weapon* w : shooters) {
      for (const Subattack& s : w->subattacks) {
        out.push_back(HitsPerVolley(*w, s, distance, radius));
      }
    }
    return out;
  };
  const auto covers = [](const std::vector<int>& a, const std::vector<int>& b) {
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a[i] < b[i]) return false;
    }
    return true;
  };
  std::vector<double> sorted = distances;
  std::ranges::sort(sorted, std::greater<>());
  // Farthest first, so of two distances that land the same, the farther
  // stays: standing back costs nothing then.
  std::vector<std::pair<double, std::vector<int>>> kept;
  for (const double d : sorted) {
    std::vector<int> hits = hits_at(d);
    if (std::ranges::any_of(kept, [&](const auto& k) { return covers(k.second, hits); })) {
      continue;
    }
    std::erase_if(kept, [&](const auto& k) { return covers(hits, k.second); });
    kept.emplace_back(d, std::move(hits));
  }
  std::vector<double> out;
  for (const auto& [d, hits] : kept) out.push_back(d);
  std::ranges::sort(out);
  return out;
}

}  // namespace rotmg
