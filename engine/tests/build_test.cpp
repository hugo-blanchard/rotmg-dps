#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "fixture.hpp"
#include "rotmg/build.hpp"

using Catch::Approx;
using rotmg::EvaluateBuild;
using rotmg::EvaluateWeapon;

namespace {

const rotmg::Dataset& Data() {
  static const rotmg::Dataset dataset = LoadSmallDataset();
  return dataset;
}

const rotmg::Weapon& Sword() { return *rotmg::FindWeapon(Data(), "Plain Sword"); }
const rotmg::Ability* Find(const char* name) { return rotmg::FindAbility(Data(), name); }

const rotmg::Character kChar{.attack = 50, .dexterity = 50, .wisdom = 60};

}  // namespace

TEST_CASE("without an ability, a build is just its weapon") {
  const auto b = EvaluateBuild(Sword(), nullptr, kChar, {}, {});
  CHECK(b.dps == Approx(EvaluateWeapon(Sword(), kChar, {}, {}).dps));
  CHECK_FALSE(b.ability.has_value());
}

TEST_CASE("a damage ability adds its DPS on top of the weapon") {
  const auto b = EvaluateBuild(Sword(), Find("Plain Spell"), kChar, {}, {});
  REQUIRE(b.ability.has_value());
  CHECK(b.weapon_dps == Approx(b.weapon.dps));
  CHECK(b.dps == Approx(b.weapon.dps + b.ability->dps));
}

TEST_CASE("a buff counts for the share of the cast cycle it's up") {
  // War Helm: Berserk for 7s at 60 WIS. 90 MP at 7.7 MP/s is a cast every
  // 11.69s, so Berserk is up 7 / 11.69 of the time.
  const auto b = EvaluateBuild(Sword(), Find("War Helm"), kChar, {}, {});
  const double period = 90 / 7.7;
  const double off = EvaluateWeapon(Sword(), kChar, {}, {}).dps;
  const double on = EvaluateWeapon(Sword(), {.attack = 50, .dexterity = 50, .wisdom = 60, .status = {.berserk = true}},
                                   {}, {})
                        .dps;
  REQUIRE(b.uptimes.size() == 1);
  CHECK(b.uptimes[0].share == Approx(7 / period));
  CHECK(b.weapon_dps == Approx((7 * on + (period - 7) * off) / period));
  CHECK(b.dps == Approx(b.weapon_dps));
}

TEST_CASE("buffs with different durations are split into pieces") {
  // Battle Seal at 64 WIS (60 + its own 4): Damaging for 5.4s, +10 ATT for 3s.
  const auto b = EvaluateBuild(Sword(), Find("Battle Seal"), kChar, {}, {});
  const rotmg::Character geared = kChar.With({{"WIS", 4}});
  const double period = 90 / rotmg::MpPerSecond(64);
  auto dps = [&](int att, bool damaging) {
    rotmg::Character c = geared;
    c.attack += att;
    c.status.damaging = damaging;
    return EvaluateWeapon(Sword(), c, {}, {}).dps;
  };
  const double expected = (3 * dps(10, true) + 2.4 * dps(0, true) + (period - 5.4) * dps(0, false)) / period;
  CHECK(b.weapon_dps == Approx(expected));
}

TEST_CASE("a curse raises the weapon's damage while it's up") {
  const auto b = EvaluateBuild(Sword(), Find("Curse Orb"), kChar, {.defense = 30}, {});
  CHECK(b.weapon_dps > b.weapon.dps);
  REQUIRE(b.uptimes.size() == 1);
  CHECK(b.uptimes[0].buff.effect == "Curse");
}

TEST_CASE("a buff can't be up longer than the time between casts") {
  rotmg::Ability always;
  always.mp_cost = 1;
  rotmg::Activate berserk;
  berserk.type = "ConditionEffectSelf";
  berserk.params.emplace("effect", std::string("Berserk"));
  berserk.params.emplace("duration", 60.0);
  always.activates.push_back(berserk);
  const auto b = EvaluateBuild(Sword(), &always, kChar, {}, {});
  CHECK(b.uptimes[0].share == 1);
  CHECK(b.weapon_dps ==
        Approx(EvaluateWeapon(Sword(), {.attack = 50, .dexterity = 50, .wisdom = 60, .status = {.berserk = true}}, {},
                              {})
                   .dps));
}

TEST_CASE("a poison's damage boost raises weapon damage while it lasts") {
  // Toxic Vial at 60 WIS: 11.75% more damage for 4.5 s of every 100 / 7.7 s.
  const auto b = EvaluateBuild(Sword(), Find("Toxic Vial"), kChar, {}, {});
  const double share = 4.5 / (100 / 7.7);
  CHECK(b.weapon_dps == Approx(b.weapon.dps * (1 + 0.1175 * share)));
}
