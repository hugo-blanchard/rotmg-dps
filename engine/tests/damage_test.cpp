#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "rotmg/damage.hpp"

using Catch::Approx;
using rotmg::DamageMultiplier;
using rotmg::EffectiveDefense;
using rotmg::PlayerStatus;
using rotmg::Target;

namespace {

rotmg::Projectile Rolls(int min_damage, int max_damage) {
  rotmg::Projectile p;
  p.min_damage = min_damage;
  p.max_damage = max_damage;
  return p;
}

}  // namespace

TEST_CASE("ATT scales damage from 50% at 0 ATT, 2% per point") {
  CHECK(DamageMultiplier(0, {}).Value() == Approx(0.5));
  CHECK(DamageMultiplier(25, {}).Value() == Approx(1.0));
  CHECK(DamageMultiplier(75, {}).Value() == Approx(2.0));
}

TEST_CASE("Weak overrides ATT and Damaging") {
  CHECK(DamageMultiplier(75, {.weak = true}).Value() == Approx(0.5));
  CHECK(DamageMultiplier(75, {.weak = true, .damaging = true}).Value() == Approx(0.5));
}

TEST_CASE("Damaging adds 25% on top of ATT") {
  CHECK(DamageMultiplier(75, {.damaging = true}).Value() == Approx(2.5));
}

TEST_CASE("hit damage is rounded down exactly") {
  // At 4 ATT the multiplier is 0.58. With doubles, 50 * (0.5 + 4 / 50.0)
  // comes out as 28.999... and would floor to 28.
  CHECK(rotmg::HitDamage(50, DamageMultiplier(4, {})) == 29);
  CHECK(rotmg::HitDamage(51, DamageMultiplier(4, {})) == 29);
  CHECK(rotmg::HitDamage(7, DamageMultiplier(0, {})) == 3);
}

TEST_CASE("Armored adds half the DEF, rounded down") {
  CHECK(EffectiveDefense({.defense = 25, .armored = true}, false) == 37);
}

TEST_CASE("Armor Broken zeroes positive DEF and beats Armored") {
  CHECK(EffectiveDefense({.defense = 40, .armor_broken = true}, false) == 0);
  CHECK(EffectiveDefense({.defense = 40, .armored = true, .armor_broken = true}, false) == 0);
  CHECK(EffectiveDefense({.defense = -5, .armor_broken = true}, false) == -5);
}

TEST_CASE("Exposed takes 20 DEF, even below zero") {
  CHECK(EffectiveDefense({.defense = 30, .exposed = true}, false) == 10);
  CHECK(EffectiveDefense({.defense = 5, .exposed = true}, false) == -15);
}

TEST_CASE("Exposed still adds 20 to armor-piercing hits and Armor Broken targets") {
  CHECK(EffectiveDefense({.defense = 60, .exposed = true}, true) == -20);
  CHECK(EffectiveDefense({.defense = 60, .armor_broken = true, .exposed = true}, false) == -20);
}

TEST_CASE("armor-piercing hits ignore DEF") {
  CHECK(EffectiveDefense({.defense = 60, .armored = true}, true) == 0);
}

TEST_CASE("DEF can't take a hit below 10%") {
  CHECK(rotmg::DamageAfterDefense(60, 20, false) == Approx(40));
  CHECK(rotmg::DamageAfterDefense(60, 54, false) == Approx(6));
  CHECK(rotmg::DamageAfterDefense(60, 200, false) == Approx(6));
  CHECK(rotmg::DamageAfterDefense(60, -20, false) == Approx(80));
}

TEST_CASE("Curse adds 25% after DEF") {
  CHECK(rotmg::DamageAfterDefense(60, 20, true) == Approx(50));
  CHECK(rotmg::DamageAfterDefense(60, 200, true) == Approx(7.5));
}

TEST_CASE("expected hit damage averages min to max - 1") {
  const rotmg::Projectile p = Rolls(10, 14);
  // Rolls 10..13 at 1x against 0 DEF.
  CHECK(rotmg::ExpectedHitDamage(p, DamageMultiplier(25, {}), {}) == Approx(11.5));
  // Against 12 DEF: 10% floors of 1, 1.1, 1.2 for rolls 10-12, then 13 - 12 = 1.
  CHECK(rotmg::ExpectedHitDamage(p, DamageMultiplier(25, {}), {.defense = 12}) == Approx((1.0 + 1.1 + 1.2 + 1.3) / 4));
}

TEST_CASE("expected hit damage handles a single possible roll") {
  CHECK(rotmg::ExpectedHitDamage(Rolls(0, 0), DamageMultiplier(50, {}), {}) == 0);
  CHECK(rotmg::ExpectedHitDamage(Rolls(100, 100), DamageMultiplier(25, {}), {}) == Approx(100));
}
