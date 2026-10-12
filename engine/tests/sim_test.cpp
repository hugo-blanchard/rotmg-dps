#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

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

TEST_CASE("turning or moving the scene doesn't change what lands") {
  // The same shot at a target to the side, behind, or with the player
  // elsewhere hits exactly as straight ahead from the origin does.
  for (const char* name : kWeapons) {
    const rotmg::Weapon& w = Weapon(name);
    for (double distance : {1.5, 3.0, 6.0}) {
      for (double degrees : {90.0, 180.0, 233.0}) {
        const double a = degrees * std::numbers::pi / 180;
        const rotmg::Point player{2, -1};
        const rotmg::Scenario scenario{
            player, {{{player.x + distance * std::sin(a), player.y + distance * std::cos(a)}, 0.5, {}}}, 0};
        Simulation sim(w, {.attack = 50, .dexterity = 50}, scenario);
        sim.Advance(10'000);
        for (std::size_t i = 0; i < w.subattacks.size(); ++i) {
          const auto& c = sim.PerSubattack()[i];
          INFO(name << ", subattack " << i + 1 << ", " << distance << " tiles at " << degrees << " degrees");
          CHECK(c.resolved_hits == rotmg::HitsPerVolley(w, w.subattacks[i], distance, 0.5) * c.resolved_volleys);
        }
      }
    }
  }
}

namespace {

// One straight shot a volley, 10 tiles/s for 1 s.
rotmg::Weapon Lance(bool pierces) {
  rotmg::Weapon w;
  rotmg::Projectile p;
  p.speed = 100;
  p.lifetime_ms = 1000;
  p.min_damage = 100;
  p.max_damage = 101;
  p.multi_hit = pierces;
  w.projectiles.push_back(p);
  w.subattacks.push_back(rotmg::Subattack{});
  return w;
}

}  // namespace

TEST_CASE("a shot that doesn't pierce stops at the first target in its way") {
  // Two dummies in a line; the player aims at the far one.
  const rotmg::Scenario line{{0, 0}, {{{0, 2}, 0.5, {}}, {{0, 5}, 0.5, {}}}, 1};
  for (bool pierces : {false, true}) {
    Simulation sim(Lance(pierces), {.attack = 25, .dexterity = 0}, line);
    sim.Advance(10'000);
    const auto& near = sim.PerTarget()[0];
    const auto& far = sim.PerTarget()[1];
    INFO((pierces ? "piercing" : "not piercing"));
    // Every volley reaches the near dummy, counting the one still flying.
    CHECK(near.hits >= sim.Totals().resolved_volleys);
    CHECK(near.hits <= sim.Totals().volleys);
    if (pierces) {
      CHECK(far.hits >= sim.Totals().resolved_volleys);
    } else {
      CHECK(far.hits == 0);
    }
  }
}

TEST_CASE("each target takes damage through its own DEF") {
  // A piercing shot through two dummies, the far one with 50 DEF: 100
  // damage at 1x becomes 50 on it.
  const rotmg::Scenario line{{0, 0}, {{{0, 2}, 0.5, {}}, {{0, 5}, 0.5, {.defense = 50}}}, 1};
  Simulation sim(Lance(true), {.attack = 25, .dexterity = 0}, line);
  sim.Advance(10'000);
  REQUIRE(sim.PerTarget()[1].hits > 0);
  CHECK(sim.PerTarget()[0].damage / sim.PerTarget()[0].hits == Catch::Approx(100));
  CHECK(sim.PerTarget()[1].damage / sim.PerTarget()[1].hits == Catch::Approx(50));
}
