#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "rotmg/fire_rate.hpp"

using Catch::Approx;
using rotmg::AttacksPerSecond;
using rotmg::BurstDelay;
using rotmg::Subattack;
using rotmg::VolleysPerSecond;

namespace {

Subattack Burst(int count, double delay, double min_delay, double rate_of_fire = 1) {
  Subattack s;
  s.rate_of_fire = rate_of_fire;
  s.burst_count = count;
  s.burst_delay = delay;
  s.burst_min_delay = min_delay;
  return s;
}

}  // namespace

TEST_CASE("DEX sets attacks per second") {
  CHECK(AttacksPerSecond(0, {}) == Approx(1.5));
  CHECK(AttacksPerSecond(75, {}) == Approx(8.0));
}

TEST_CASE("Berserk adds 25% attack speed, Dazed cancels it") {
  CHECK(AttacksPerSecond(75, {.berserk = true}) == Approx(10.0));
  CHECK(AttacksPerSecond(75, {.dazed = true}) == Approx(1.5));
  CHECK(AttacksPerSecond(75, {.berserk = true, .dazed = true}) == Approx(1.5));
}

TEST_CASE("rate of fire scales volleys") {
  Subattack s;
  s.rate_of_fire = 0.5;
  CHECK(VolleysPerSecond(s, 75, {}) == Approx(4.0));
}

TEST_CASE("burst delay moves from max to min between 0 and 75 DEX, then stops") {
  const Subattack s = Burst(4, 1.8, 0.8);
  CHECK(BurstDelay(s, 0, {}) == Approx(1.8));
  CHECK(BurstDelay(s, 37, {}) == Approx(1.8 - 1.0 * 37 / 75));
  CHECK(BurstDelay(s, 75, {}) == Approx(0.8));
  CHECK(BurstDelay(s, 100, {}) == Approx(0.8));
}

TEST_CASE("Berserk shortens burst delay, Dazed uses the 0 DEX delay") {
  const Subattack s = Burst(4, 1.8, 0.8);
  CHECK(BurstDelay(s, 75, {.berserk = true}) == Approx(0.8 * 0.67));
  CHECK(BurstDelay(s, 75, {.dazed = true}) == Approx(1.8));
}

TEST_CASE("a burst caps volleys at burst count per burst delay") {
  // 8 volleys/s uncapped, 4 per 0.8 s = 5 per second.
  CHECK(VolleysPerSecond(Burst(4, 1.8, 0.8), 75, {}) == Approx(5.0));
  // At 0 DEX: 1.5/s uncapped vs 4 per 1.8 s = 2.22/s, so the cap doesn't bite.
  CHECK(VolleysPerSecond(Burst(4, 1.8, 0.8), 0, {}) == Approx(1.5));
}

TEST_CASE("a burst without a min delay uses the max delay at any DEX") {
  Subattack s = Burst(3, 1.2, 0);
  s.burst_min_delay.reset();
  CHECK(BurstDelay(s, 75, {}) == Approx(1.2));
}
