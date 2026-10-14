// Invariants that must hold for every weapon, checked across a sweep of
// stats and targets on the fixture weapons.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "fixture.hpp"
#include "rotmg/build.hpp"
#include "rotmg/enchant.hpp"
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

TEST_CASE("damage and fire rate enchantments never lower the projectiles' DPS") {
  // Procs are left out for the same reason as with DEX.
  std::vector<const rotmg::Enchantment*> bonuses;
  for (const rotmg::Enchantment& e : Data().enchantments) {
    if (e.id == "Damage_Bonus_4" || e.id == "FireRate_Bonus_4") {
      bonuses.push_back(&e);
    }
  }
  REQUIRE(bonuses.size() == 2);
  for (const rotmg::Weapon& w : Data().weapons) {
    for (const rotmg::Enchantment* e : bonuses) {
      const rotmg::Weapon enchanted = rotmg::Enchant(w, {e}).weapon;
      for (int def : kDefenses) {
        for (int att = 0; att <= 100; att += 5) {
          const auto projectiles = [&](const rotmg::Weapon& weapon) {
            double dps = 0;
            for (const rotmg::SubattackResult& r :
                 EvaluateWeapon(weapon, {.attack = att, .dexterity = 50}, {.defense = def}).subattacks) {
              dps += r.dps;
            }
            return dps;
          };
          const double plain = projectiles(w);
          const double better = projectiles(enchanted);
          INFO(w.name << " with " << e->name << ", ATT " << att << ", DEF " << def);
          CHECK(better >= plain);
        }
      }
    }
  }
}

TEST_CASE("more of any stat never lowers a build's sustained DPS") {
  // What the optimizer relies on to drop a build that has less of every
  // stat than another. Every weapon with every ability, and with none.
  constexpr std::array<int rotmg::Character::*, 8> kStats = {
      &rotmg::Character::attack,  &rotmg::Character::dexterity, &rotmg::Character::speed,
      &rotmg::Character::wisdom,  &rotmg::Character::vitality,  &rotmg::Character::defense,
      &rotmg::Character::max_hit_points, &rotmg::Character::max_magic_points};
  std::vector<const rotmg::Ability*> abilities = {nullptr};
  for (const rotmg::Ability& a : Data().abilities) abilities.push_back(&a);
  for (const rotmg::Weapon& w : Data().weapons) {
    for (const rotmg::Ability* a : abilities) {
      for (int rotmg::Character::*stat : kStats) {
        rotmg::Character c{.attack = 50, .dexterity = 50, .wisdom = 50, .vitality = 40, .defense = 25, .speed = 50,
                           .max_hit_points = 700, .max_magic_points = 400};
        double previous = 0;
        for (int extra = 0; extra <= 150; ++extra) {
          rotmg::Character more = c;
          more.*stat += extra;
          const double dps = rotmg::EvaluateBuild(w, a, more, {.defense = 30}, {}).dps;
          INFO(w.name << " with " << (a ? a->name : "no ability") << ", +" << extra);
          CHECK(dps >= previous * (1 - 1e-12));
          previous = dps;
        }
      }
    }
  }
}
