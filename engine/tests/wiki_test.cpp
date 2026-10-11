// Numbers documented on the RealmEye wiki, checked against the engine.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "rotmg/damage.hpp"
#include "rotmg/fire_rate.hpp"

using Catch::Approx;
using rotmg::DamageMultiplier;
using rotmg::HitDamage;

TEST_CASE("Character Stats: 83 ATT Warrior with a Sword of Acclaim does 475-591") {
  // 220-275 listed; 275 is never rolled.
  const auto m = DamageMultiplier(83, {});
  CHECK(HitDamage(220, m) == 475);
  CHECK(HitDamage(274, m) == 591);
}

TEST_CASE("Character Stats: 10 ATT Priest with a Fire Wand does 21-37") {
  // 30-55 listed; 55 is never rolled.
  const auto m = DamageMultiplier(10, {});
  CHECK(HitDamage(30, m) == 21);
  CHECK(HitDamage(54, m) == 37);
}

TEST_CASE("Character Stats: a 60 damage hit against 20 DEF does 40, against 54 or more does 6") {
  CHECK(rotmg::DamageAfterDefense(60, 20, false) == Approx(40));
  for (int def : {54, 60, 100}) {
    CHECK(rotmg::DamageAfterDefense(60, def, false) == Approx(6));
  }
}

TEST_CASE("Character Stats: 50 DEX is 5.833 attacks per second, 75 DEX is 8") {
  CHECK(rotmg::AttacksPerSecond(50, {}) == Approx(5.833).epsilon(0.001));
  CHECK(rotmg::AttacksPerSecond(75, {}) == Approx(8.0));
}

TEST_CASE("Longbows: burst delay is (135 - DEX) / 75 seconds") {
  rotmg::Subattack longbow;
  longbow.burst_count = 4;
  longbow.burst_delay = 1.8;
  longbow.burst_min_delay = 0.8;
  for (int dex = 0; dex <= 75; dex += 5) {
    INFO("DEX " << dex);
    CHECK(rotmg::BurstDelay(longbow, dex, {}) == Approx((135.0 - dex) / 75));
  }
}
