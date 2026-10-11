#include <catch2/catch_test_macros.hpp>

#include "rotmg/hits.hpp"

using rotmg::HitsPerVolley;
using rotmg::Projectile;
using rotmg::Subattack;
using rotmg::Weapon;

namespace {

// One projectile type, one subattack. Speed is in the client's tenths of a
// tile per second.
Weapon Single(double speed, double lifetime_ms, int shots = 1) {
  Weapon w;
  Projectile p;
  p.speed = speed;
  p.lifetime_ms = lifetime_ms;
  w.projectiles.push_back(p);
  Subattack s;
  s.num_projectiles = shots;
  w.subattacks.push_back(s);
  return w;
}

int Hits(const Weapon& w, double distance, double radius = rotmg::kDefaultTargetRadius) {
  return HitsPerVolley(w, w.subattacks[0], distance, radius);
}

}  // namespace

TEST_CASE("a straight shot hits anything within its range") {
  const Weapon sword = Single(100, 350);  // 3.5 tiles
  CHECK(Hits(sword, 1) == 1);
  CHECK(Hits(sword, 3.9) == 1);  // the target's edge is within range
  CHECK(Hits(sword, 4.1) == 0);
}

TEST_CASE("side shots of a spread miss once the target is narrower than the gap") {
  // Three shots 8 degrees apart. A side shot passes the target's center at
  // distance d * sin(8 deg), so it misses past 0.5 / sin(8 deg) = 3.59 tiles.
  Weapon bow = Single(160, 440, 3);
  bow.subattacks[0].arc_gap = 8;
  CHECK(Hits(bow, 3.5) == 3);
  CHECK(Hits(bow, 3.7) == 1);
  CHECK(Hits(bow, 7.6) == 0);
}

TEST_CASE("a missing arc gap means 11.25 degrees") {
  Weapon w = Single(160, 440, 2);
  // Each shot is 5.625 degrees off center: 0.5 / sin(5.625 deg) = 5.1 tiles.
  CHECK(Hits(w, 5.0) == 2);
  CHECK(Hits(w, 5.2) == 0);
}

TEST_CASE("offset shots angled inward hit more as the target gets farther") {
  Weapon w = Single(160, 440);
  w.subattacks[0].pos_offset = std::array<double, 2>{-1.0, 0.0};
  w.subattacks[0].default_angle = 6;
  Subattack mirror = w.subattacks[0];
  mirror.pos_offset = std::array<double, 2>{1.0, 0.0};
  mirror.default_angle = -6;
  w.subattacks.push_back(mirror);
  const auto total = [&](double d) {
    return HitsPerVolley(w, w.subattacks[0], d, 0.5) + HitsPerVolley(w, w.subattacks[1], d, 0.5);
  };
  CHECK(total(2) == 0);
  CHECK(total(6) == 2);
}

TEST_CASE("deceleration with a clamp of 0 stops the shot short") {
  // 12 tiles/s slowing by 12 tiles/s^2 stops after 6 tiles.
  Weapon w = Single(120, 1700);
  w.projectiles[0].acceleration = -120;
  w.projectiles[0].acceleration_delay = 0;
  w.projectiles[0].speed_clamp = 0;
  CHECK(Hits(w, 6.3) == 1);
  CHECK(Hits(w, 7) == 0);
}

TEST_CASE("boomerang shots only reach half their straight-line range") {
  Weapon w = Single(100, 700);  // 7 tiles straight, 3.5 out and back
  w.projectiles[0].boomerang = true;
  CHECK(Hits(w, 3.9) == 1);
  CHECK(Hits(w, 4.2) == 0);
}

TEST_CASE("sine-wave shots cross the aim line where the wave does") {
  // 8.55 tiles of range, 2 cycles: the wave crosses zero every 2.1375 tiles
  // and peaks 0.5 tiles off the aim line halfway between.
  Weapon staff = Single(180, 475, 2);
  staff.subattacks[0].arc_gap = 0;
  staff.projectiles[0].amplitude = 0.5;
  staff.projectiles[0].frequency = 2;
  CHECK(Hits(staff, 2.1375, 0.05) == 2);
  CHECK(Hits(staff, 1.06875, 0.1) == 0);
  CHECK(Hits(staff, 1.06875, 0.5) == 2);
}

TEST_CASE("a stationary projectile only hits a target on top of the player") {
  const Weapon w = Single(0, 1000);
  CHECK(Hits(w, 0.3) == 1);
  CHECK(Hits(w, 1) == 0);
}

TEST_CASE("weapons whose movement isn't modelled are reported") {
  Weapon w = Single(100, 350);
  CHECK(rotmg::HitModelGaps(w).empty());
  w.projectiles[0].parametric = true;
  CHECK(rotmg::HitModelGaps(w).size() == 1);
  w.projectiles[0].parametric = false;
  w.unsupported = {"OnPlayerShootActivate"};
  CHECK(rotmg::HitModelGaps(w).empty());
  w.unsupported.push_back("TurnRate");
  CHECK(rotmg::HitModelGaps(w).size() == 1);
}

TEST_CASE("placeholder projectiles with no lifetime never hit") {
  const Weapon w = Single(0, 0);
  CHECK(Hits(w, 0.3) == 0);
}
