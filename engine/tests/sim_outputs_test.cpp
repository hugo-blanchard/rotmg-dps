#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numeric>
#include <vector>

#include "fixture.hpp"
#include "rotmg/evaluate.hpp"
#include "rotmg/sim.hpp"

using Catch::Approx;
using rotmg::Scenario;
using rotmg::Simulation;

namespace {

const rotmg::Weapon& Sword() {
  static const rotmg::Dataset dataset = LoadSmallDataset();
  const rotmg::Weapon* w = rotmg::FindWeapon(dataset, "Plain Sword");
  REQUIRE(w != nullptr);
  return *w;
}

const rotmg::Character kCharacter{.attack = 50, .dexterity = 50};

// A target with 1000 HP 2 tiles ahead, aimed at, and a dummy off to the side.
Scenario Mortal() {
  Scenario s;
  s.targets.push_back({{0, 2}, 0.5, {}, {}, 1000});
  s.targets.push_back({{2, 0}, 0.5, {}, {}, 0});
  return s;
}

double Total(const Simulation& sim) {
  double total = 0;
  for (const auto& t : sim.PerTarget()) {
    total += t.damage;
  }
  return total;
}

}  // namespace

TEST_CASE("a target dies at 0 HP and the player turns to the next") {
  // The sword does about 564 DPS here, so 1000 HP lasts under 2 seconds.
  Simulation sim(Sword(), kCharacter, Mortal());
  sim.Advance(5000);
  const auto& dead = sim.PerTarget()[0];
  CHECK(dead.damage == Approx(1000));
  CHECK(dead.hp == 0);
  CHECK(dead.kills == 1);
  CHECK(dead.killed_ms == Approx(1000 / rotmg::EvaluateWeapon(Sword(), kCharacter, {}).dps * 1000).margin(200));
  CHECK(sim.World().aim == 1);
  CHECK(sim.PerTarget()[1].hits > 0);
}

TEST_CASE("a target that respawns keeps taking damage") {
  Scenario s = Mortal();
  s.respawn = true;
  Simulation sim(Sword(), kCharacter, s);
  sim.Advance(5000);
  const auto& t = sim.PerTarget()[0];
  CHECK(t.kills >= 2);
  CHECK(t.damage > 2000);
  CHECK(t.hp > 0);
  CHECK(sim.World().aim == 0);
}

TEST_CASE("random rolls repeat with the seed and average out") {
  Scenario s;
  s.targets.push_back({{0, 2}, 0.5, {}});
  s.random_rolls = true;
  const auto run = [&](unsigned long long seed, double ms) {
    s.seed = seed;
    Simulation sim(Sword(), kCharacter, s);
    sim.Advance(ms);
    return sim.Totals().damage;
  };
  CHECK(run(1, 5000) == run(1, 5000));
  CHECK(run(1, 5000) != run(2, 5000));
  CHECK(run(3, 300'000) / 300 == Approx(rotmg::EvaluateWeapon(Sword(), kCharacter, {}).dps).epsilon(0.01));
}

TEST_CASE("the history holds damage and MP every 100 ms") {
  Simulation sim(Sword(), kCharacter, Mortal());
  sim.Advance(10'000);
  const auto& damage = sim.DamageHistory();
  CHECK(damage.size() == 100);
  CHECK(sim.MpHistory().size() == 100);
  CHECK(std::accumulate(damage.begin(), damage.end(), 0.0) == Approx(Total(sim)));
}

TEST_CASE("recent hits list what landed since the last advance") {
  Simulation sim(Sword(), kCharacter, Mortal());
  sim.Advance(1000);
  const long before = sim.PerTarget()[0].hits;
  CHECK(static_cast<long>(sim.RecentHits().size()) == before);
  sim.Advance(500);
  CHECK(static_cast<long>(sim.RecentHits().size()) == sim.PerTarget()[0].hits - before);
}
