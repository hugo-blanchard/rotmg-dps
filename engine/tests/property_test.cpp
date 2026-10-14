// Invariants that must hold for every weapon, checked across a sweep of
// stats and targets on the fixture weapons.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

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

TEST_CASE("more of any stat never lowers a build's weighted DPS") {
  // The same for searches that only count the weapon or only the ability:
  // the cast is timed for what's counted, so more of a stat can't move it
  // to a rhythm that counts less.
  constexpr std::array<int rotmg::Character::*, 8> kStats = {
      &rotmg::Character::attack,  &rotmg::Character::dexterity, &rotmg::Character::speed,
      &rotmg::Character::wisdom,  &rotmg::Character::vitality,  &rotmg::Character::defense,
      &rotmg::Character::max_hit_points, &rotmg::Character::max_magic_points};
  for (const rotmg::DpsWeights weights : {rotmg::DpsWeights{1, 0}, rotmg::DpsWeights{0, 1}}) {
    for (const rotmg::Weapon& w : Data().weapons) {
      for (const rotmg::Ability& a : Data().abilities) {
        for (int rotmg::Character::*stat : kStats) {
          rotmg::Character c{.attack = 50, .dexterity = 50, .wisdom = 50, .vitality = 40, .defense = 25,
                             .speed = 50, .max_hit_points = 700, .max_magic_points = 400};
          double previous = 0;
          for (int extra = 0; extra <= 150; extra += 3) {
            rotmg::Character more = c;
            more.*stat += extra;
            const rotmg::BuildNumbers n = rotmg::EvaluateBuildNumbers(w, &a, more, {.defense = 30}, {}, weights);
            const double weighted = weights.weapon * n.weapon_dps + weights.ability * n.ability_dps;
            INFO(w.name << " with " << a.name << ", +" << extra);
            CHECK(weighted >= previous * (1 - 1e-12));
            previous = weighted;
          }
        }
      }
    }
  }
}

namespace {

// The DPS a search counts under `weights`.
double Weighted(const rotmg::Weapon& w, const rotmg::Ability* a, const rotmg::DpsWeights& weights) {
  const rotmg::Character c{.attack = 55, .dexterity = 55, .wisdom = 55, .vitality = 40, .defense = 25, .speed = 50,
                           .max_hit_points = 700, .max_magic_points = 400};
  const rotmg::BuildNumbers n = rotmg::EvaluateBuildNumbers(w, a, c, {.defense = 30}, {}, weights);
  return weights.weapon * n.weapon_dps + weights.ability * n.ability_dps;
}

std::vector<const rotmg::Ability*> AbilitiesAndNone() {
  std::vector<const rotmg::Ability*> out = {nullptr};
  for (const rotmg::Ability& a : Data().abilities) out.push_back(&a);
  return out;
}

constexpr std::array<rotmg::DpsWeights, 3> kWeights = {rotmg::DpsWeights{1, 1}, rotmg::DpsWeights{1, 0},
                                                       rotmg::DpsWeights{0, 1}};

}  // namespace

TEST_CASE("a stronger stacking chance boost never lowers a build's DPS") {
  // What the exact search relies on to drop a weaker boost for a stronger
  // one: more amount, duration or chance, or less cooldown.
  struct Sweep {
    const char* key;
    double from;
    double to;
    double step;
  };
  const Sweep sweeps[] = {{"amount", 0, 30, 3}, {"duration", 0.5, 6, 0.5}, {"proc", 0.05, 0.95, 0.05},
                          {"cooldown", 10, 0, -1}};
  for (const rotmg::DpsWeights& weights : kWeights) {
    for (const rotmg::Weapon& w : Data().weapons) {
      for (const rotmg::Ability* a : AbilitiesAndNone()) {
        for (const char* stat : {"ATT", "DEX"}) {
          for (const Sweep& sweep : sweeps) {
            double previous = 0;
            for (double v = sweep.from; sweep.step > 0 ? v <= sweep.to + 1e-9 : v >= sweep.to - 1e-9; v += sweep.step) {
              rotmg::Activate boost;
              boost.type = "StatBoostSelf";
              boost.params = {{"stat", std::string(stat)}, {"amount", 10.0}, {"duration", 3.0}, {"proc", 0.2},
                              {"cooldown", 4.0}};
              boost.params[sweep.key] = v;
              rotmg::Weapon boosted = w;
              boosted.on_shoot.push_back(boost);
              const double dps = Weighted(boosted, a, weights);
              INFO(w.name << " with " << (a ? a->name : "no ability") << ", " << stat << " " << sweep.key << " " << v);
              CHECK(dps >= previous * (1 - 1e-12));
              previous = dps;
            }
          }
        }
      }
    }
  }
}

TEST_CASE("a lower MP cost never lowers a build's DPS") {
  for (const rotmg::DpsWeights& weights : kWeights) {
    for (const rotmg::Weapon& w : Data().weapons) {
      for (const rotmg::Ability& a : Data().abilities) {
        if (!a.mp_cost) continue;
        double previous = 0;
        for (double share = 1.5; share >= 0.3; share -= 0.05) {
          rotmg::Ability cheaper = a;
          cheaper.mp_cost = *a.mp_cost * share;
          const double dps = Weighted(w, &cheaper, weights);
          INFO(w.name << " with " << a.name << ", MP cost x" << share);
          CHECK(dps >= previous * (1 - 1e-12));
          previous = dps;
        }
      }
    }
  }
}

TEST_CASE("higher damage and fire rate multipliers never lower a build's DPS") {
  // The build as a whole, procs and chance boosts included.
  for (const char* tag : {"MultiplyMinDamage", "MultiplyMaxDamage", "MultiplyRateOfFire"}) {
    for (const rotmg::DpsWeights& weights : kWeights) {
      for (const rotmg::Weapon& w : Data().weapons) {
        for (const rotmg::Ability* a : AbilitiesAndNone()) {
          double previous = 0;
          for (double factor = 0.6; factor <= 1.6 + 1e-9; factor += 0.05) {
            rotmg::Enchantment e;
            e.id = "Test";
            e.name = "Test";
            rotmg::Mutator m;
            m.tag = tag;
            m.number = factor;
            m.attributes.params.emplace(tag == std::string("MultiplyRateOfFire") ? "subAttackIndex" : "projectileId",
                                        -1.0);
            e.mutators.push_back(m);
            const double dps = Weighted(rotmg::Enchant(w, {&e}).weapon, a, weights);
            INFO(w.name << " with " << (a ? a->name : "no ability") << ", " << tag << " " << factor);
            CHECK(dps >= previous * (1 - 1e-12));
            previous = dps;
          }
        }
      }
    }
  }
}
