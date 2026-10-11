// Invariants that must hold for every weapon, checked across a sweep of
// stats and targets on the fixture weapons.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "fixture.hpp"
#include "rotmg/evaluate.hpp"
#include "rotmg/fire_rate.hpp"

using rotmg::EvaluateWeapon;

namespace {

const rotmg::Dataset& Data() {
  static const rotmg::Dataset dataset = LoadSmallDataset();
  return dataset;
}

const int kDefenses[] = {0, 10, 25, 50, 100, 200};

}  // namespace

TEST_CASE("more ATT never lowers DPS") {
  for (const rotmg::Weapon& w : Data().weapons) {
    for (int def : kDefenses) {
      double previous = 0;
      for (int att = 0; att <= 100; ++att) {
        const double dps = EvaluateWeapon(w, {.attack = att, .dexterity = 50}, {.defense = def}).dps;
        INFO(w.name << ", ATT " << att << ", DEF " << def);
        CHECK(dps >= previous);
        previous = dps;
      }
    }
  }
}

TEST_CASE("more DEX never lowers the projectiles' DPS") {
  // Procs are left out: a burst waits for the first shot after its
  // cooldown, so a little more DEX can mean slightly fewer bursts a second.
  for (const rotmg::Weapon& w : Data().weapons) {
    double previous = 0;
    for (int dex = 0; dex <= 100; ++dex) {
      const auto b = EvaluateWeapon(w, {.attack = 50, .dexterity = dex}, {.defense = 20});
      double dps = 0;
      for (const rotmg::SubattackResult& s : b.subattacks) {
        dps += s.dps;
      }
      INFO(w.name << ", DEX " << dex);
      CHECK(dps >= previous);
      previous = dps;
    }
  }
}

TEST_CASE("more DEF never raises DPS") {
  for (const rotmg::Weapon& w : Data().weapons) {
    for (bool armored : {false, true}) {
      double previous = EvaluateWeapon(w, {.attack = 50, .dexterity = 50}, {.defense = 0, .armored = armored}).dps;
      for (int def = 1; def <= 200; ++def) {
        const double dps = EvaluateWeapon(w, {.attack = 50, .dexterity = 50}, {.defense = def, .armored = armored}).dps;
        INFO(w.name << ", DEF " << def << (armored ? ", Armored" : ""));
        CHECK(dps <= previous);
        previous = dps;
      }
    }
  }
}

TEST_CASE("armor-piercing damage doesn't depend on DEF") {
  for (const rotmg::Weapon& w : Data().weapons) {
    for (const rotmg::Projectile& p : w.projectiles) {
      if (!p.armor_piercing) {
        continue;
      }
      const auto m = rotmg::DamageMultiplier(50, {});
      const double base = rotmg::ExpectedHitDamage(p, m, {});
      for (int def : kDefenses) {
        INFO(w.name << " projectile " << p.id << ", DEF " << def);
        CHECK(rotmg::ExpectedHitDamage(p, m, {.defense = def, .armored = true}) == Catch::Approx(base));
      }
    }
  }
}

TEST_CASE("a hit never drops below 10% of its damage") {
  for (int damage = 0; damage <= 1000; damage += 7) {
    for (int def : kDefenses) {
      INFO("damage " << damage << ", DEF " << def);
      CHECK(rotmg::DamageAfterDefense(damage, def, false) >= 0.1 * damage);
    }
  }
}

TEST_CASE("a burst never fires faster than the same weapon without one") {
  for (const rotmg::Weapon& w : Data().weapons) {
    for (const rotmg::Subattack& s : w.subattacks) {
      rotmg::Subattack unlimited = s;
      unlimited.burst_count.reset();
      for (int dex = 0; dex <= 100; dex += 5) {
        for (bool berserk : {false, true}) {
          const rotmg::PlayerStatus status{.berserk = berserk};
          INFO(w.name << ", DEX " << dex << (berserk ? ", Berserk" : ""));
          CHECK(rotmg::VolleysPerSecond(s, dex, status) <= rotmg::VolleysPerSecond(unlimited, dex, status));
        }
      }
    }
  }
}
