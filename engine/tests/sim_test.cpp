#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "fixture.hpp"
#include "rotmg/hits.hpp"
#include "rotmg/sim.hpp"

using rotmg::Simulation;

namespace {

const rotmg::Dataset& Data() {
  static const rotmg::Dataset dataset = LoadSmallDataset();
  return dataset;
}

const rotmg::Weapon& Weapon(const char* name) {
  const rotmg::Weapon* w = rotmg::FindWeapon(Data(), name);
  REQUIRE(w != nullptr);
  return *w;
}

const char* const kWeapons[] = {"Plain Sword", "Burst Bow", "Proc Wand", "Venom Katana"};
const double kDistances[] = {0.5, 1.5, 3.0, 4.2, 6.0, 7.4, 9.0};

}  // namespace

TEST_CASE("simulated volleys land exactly the hits the estimate predicts") {
  for (const char* name : kWeapons) {
    const rotmg::Weapon& w = Weapon(name);
    for (double distance : kDistances) {
      Simulation sim(w, {.attack = 50, .dexterity = 50}, {}, distance, 0.5);
      sim.Advance(10'000);
      for (std::size_t i = 0; i < w.subattacks.size(); ++i) {
        const auto& c = sim.PerSubattack()[i];
        INFO(name << ", subattack " << i + 1 << ", " << distance << " tiles");
        REQUIRE(c.resolved_volleys > 10);
        const int estimate = rotmg::HitsPerVolley(w, w.subattacks[i], distance, 0.5);
        CHECK(c.resolved_hits == estimate * c.resolved_volleys);
      }
    }
  }
}

TEST_CASE("simulated DPS settles on the evaluated DPS") {
  for (const char* name : kWeapons) {
    for (int dex : {0, 37, 75}) {
      for (bool berserk : {false, true}) {
        const rotmg::Character character{.attack = 60, .dexterity = dex, .status = {.berserk = berserk}};
        const rotmg::Target target{.defense = 20};
        rotmg::Assumptions a;
        a.distance = 3;
        // The simulation fires projectiles only, not procs.
        double expected = 0;
        for (const rotmg::SubattackResult& s : rotmg::EvaluateWeapon(Weapon(name), character, target, a).subattacks) {
          expected += s.dps;
        }

        Simulation sim(Weapon(name), character, target, 3, 0.5);
        sim.Advance(120'000);
        const double measured = sim.Totals().damage / (sim.TimeMs() / 1000);
        INFO(name << ", DEX " << dex << (berserk ? ", Berserk" : ""));
        CHECK(measured == Catch::Approx(expected).epsilon(0.01));
      }
    }
  }
}

TEST_CASE("the simulation is deterministic however time is split") {
  Simulation once(Weapon("Burst Bow"), {.attack = 50, .dexterity = 60}, {}, 4, 0.5);
  Simulation many(Weapon("Burst Bow"), {.attack = 50, .dexterity = 60}, {}, 4, 0.5);
  once.Advance(5'000);
  for (int i = 0; i < 300; ++i) {
    many.Advance(5'000.0 / 300);
  }
  CHECK(once.TimeMs() == many.TimeMs());
  CHECK(once.Totals().hits == many.Totals().hits);
  CHECK(once.Totals().damage == many.Totals().damage);
  CHECK(once.Shots().size() == many.Shots().size());
}

TEST_CASE("bursts pause between groups of volleys") {
  // Burst Bow at 75 DEX: volleys every 1000 / (8 * 0.9) = 139 ms, 4 per
  // burst, bursts 800 ms apart.
  Simulation sim(Weapon("Burst Bow"), {.attack = 50, .dexterity = 75}, {}, 4, 0.5);
  sim.Advance(799);
  CHECK(sim.PerSubattack()[0].volleys == 4);
  sim.Advance(2);
  CHECK(sim.PerSubattack()[0].volleys == 5);
}

TEST_CASE("shots that don't pierce disappear when they hit") {
  // Plain Sword's bolt (10 tiles/s) reaches a target 0.6 tiles away within
  // 20 ms, long before the next volley at 0 DEX.
  Simulation sim(Weapon("Plain Sword"), {.attack = 50, .dexterity = 0}, {}, 0.6, 0.5);
  sim.Advance(20);
  CHECK(sim.Totals().hits == 1);
  CHECK(sim.Shots().empty());
}
