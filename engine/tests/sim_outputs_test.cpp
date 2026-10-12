#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numeric>
#include <vector>

#include "fixture.hpp"
#include "rotmg/ability.hpp"
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

TEST_CASE("a target takes nothing while it's invulnerable") {
  // A second in every two, from 2 s on: over 10 s it's open 6 s of them.
  Scenario open;
  open.targets.push_back({{0, 2}, 0.5, {}});
  Scenario phased = open;
  phased.targets[0].invulnerable_every_s = 2;
  phased.targets[0].invulnerable_s = 1;
  Simulation a(Sword(), kCharacter, open);
  Simulation b(Sword(), kCharacter, phased);
  a.Advance(10'000);
  b.Advance(10'000);
  CHECK(b.PerTarget()[0].hits == a.PerTarget()[0].hits);
  CHECK(b.PerTarget()[0].damage / a.PerTarget()[0].damage == Approx(0.6).margin(0.03));
}

TEST_CASE("a target that respawns is a new enemy, without the old one's poison") {
  // The vial's 380 impact kills a 300 HP target out of the sword's reach;
  // the poison it would go on to deal isn't carried to the new one.
  static const rotmg::Dataset dataset = LoadSmallDataset();
  Scenario s;
  s.targets.push_back({{0, 8}, 0.5, {}, {}, 300});
  s.respawn = true;
  Simulation sim(Sword(), {.attack = 50, .dexterity = 50, .wisdom = 60}, s, rotmg::FindAbility(dataset, "Toxic Vial"));
  sim.Advance(3000);
  CHECK(sim.PerTarget()[0].kills == 1);
  CHECK(sim.PerTarget()[0].damage == Approx(300));
  CHECK(sim.TargetStatuses(0).empty());
}
