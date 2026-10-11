#pragma once

#include <vector>

#include "rotmg/model.hpp"

// Where a projectile is at each moment of its life. The hit estimate and the
// simulation both move projectiles with ProjectilePath, so they always agree.
// Motion rules come from the RealmEye wiki page Weapon Attributes.
namespace rotmg {

/// Time step for projectile motion. Small enough that the fastest projectiles
/// move about 0.05 tiles a step; hits are found on the segment between
/// steps, so nothing slips through.
inline constexpr double kStepMs = 2.0;

struct Point {
  double x = 0;
  double y = 0;
};

/// The path of one projectile of a volley, fired by a player at the origin
/// aiming along +y. A positive angle turns toward +x, and PosOffset is
/// (sideways, forward) in the same frame: that's the reading under which the
/// Hama Yumi's and Torrential Staff's offset shots converge, as the wiki says
/// they do.
class ProjectilePath {
 public:
  /// `index` is the projectile's place in the volley, from 0.
  ProjectilePath(const Projectile& projectile, const Subattack& subattack, int index);

  /// Steps of kStepMs in the projectile's life. 0 for placeholder projectiles
  /// with no lifetime, which never exist long enough to hit anything.
  int Steps() const { return steps_; }

  Point At(int step) const;

  /// The direction it was fired in, in degrees from the aim line.
  double HeadingDegrees() const { return degrees_; }

  /// Whether the projectile touches a round target during `step`: at the
  /// spawn point for step 0, or anywhere on the way from the previous step.
  bool Touches(int step, Point target, double radius) const;

 private:
  const Projectile* projectile_;
  int steps_ = 0;
  double degrees_ = 0;
  Point offset_;
  Point heading_;
  Point side_;
  double polarity_ = 1;
  std::vector<double> forward_;
};

}  // namespace rotmg
