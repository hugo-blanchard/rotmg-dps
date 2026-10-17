#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("more WIS never lowers DPS when an item's effect on casting has a cooldown") {
  // +30 ATT for 6s on casting, at most every 20s. Casting every 10.4s it
  // goes off every second cast; a little more WIS and every third, unless
  // the cast is held back to every 10s.
  rotmg::Ability spell = *Find("Plain Spell");
  rotmg::Activate boost;
  boost.type = "StatBoostSelf";
  boost.params.emplace("stat", std::string("ATT"));
  boost.params.emplace("amount", 30.0);
  boost.params.emplace("duration", 6.0);
  boost.params.emplace("cooldown", 20.0);
  spell.on_ability.push_back(boost);
  double previous = 0;
  bool held = false;
  for (int wis = 40; wis <= 150; ++wis) {
    const auto b = EvaluateBuild(Sword(), &spell, {.attack = 50, .dexterity = 50, .wisdom = wis}, {}, {});
    INFO("WIS " << wis);
    CHECK(b.dps >= previous * (1 - 1e-12));
    previous = b.dps;
    held = held || std::ranges::any_of(b.ability->assumptions, [](const std::string& a) {
             return a.find("held back") != std::string::npos;
           });
  }
  CHECK(held);
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

namespace {

// Damage in one volley of the sword's only subattack, and how many volleys
// it fires from the start of a window this long.
std::pair<double, int> Volleys(const rotmg::Character& c, double length) {
  const auto w = EvaluateWeapon(Sword(), c, {}, {});
  const double rate = w.subattacks[0].volleys_per_second;
  return {w.subattacks[0].dps / rate, static_cast<int>(std::floor(length * rate)) + 1};
}

rotmg::Character WithMp(int mp) {
  rotmg::Character c = kChar;
  c.max_magic_points = mp;
  return c;
}

}  // namespace

TEST_CASE("a burst without an ability is the weapon's volleys alone") {
  const auto b = rotmg::EvaluateBurst(Sword(), nullptr, WithMp(200), {}, {}, {0, 10});
  CHECK(b.casts == 0);
  CHECK(b.ability_damage == 0);
  CHECK(b.weapon_damage == Approx(Volleys(kChar, 0).first));
}

TEST_CASE("a burst in a single moment is one volley and one cast") {
  const auto b = rotmg::EvaluateBurst(Sword(), Find("Plain Spell"), WithMp(200), {}, {}, {0, 20});
  const auto build = EvaluateBuild(Sword(), Find("Plain Spell"), WithMp(200), {}, {});
  CHECK(b.casts == Approx(1));
  CHECK(b.weapon_damage == Approx(Volleys(kChar, 0).first));
  CHECK(b.ability_damage == Approx(build.ability->damage_per_cast));
  CHECK(b.damage == Approx(b.weapon_damage + b.ability_damage));
}

TEST_CASE("a burst's casts are limited by the MP that regenerates between windows") {
  // 200 MP holds two 80 MP casts in 3 seconds, but 7.7 MP/s over 10 seconds
  // only pays for 0.9625 of one per window in the long run.
  const auto b = rotmg::EvaluateBurst(Sword(), Find("Plain Spell"), WithMp(200), {}, {}, {3, 10});
  const auto build = EvaluateBuild(Sword(), Find("Plain Spell"), WithMp(200), {}, {});
  const auto [per_volley, volleys] = Volleys(kChar, 3);
  CHECK(b.casts == Approx(77.0 / 80));
  CHECK(b.ability_damage == Approx(77.0 / 80 * build.ability->damage_per_cast));
  CHECK(b.weapon_damage == Approx(volleys * per_volley));

  // With 30 seconds between windows, 231 MP regenerates, enough for both.
  const auto rested = rotmg::EvaluateBurst(Sword(), Find("Plain Spell"), WithMp(200), {}, {}, {3, 30});
  CHECK(rested.casts == Approx(2));
  CHECK(rested.ability_damage == Approx(2 * build.ability->damage_per_cast));
}

TEST_CASE("a burst's buffs are up from the window's first volley") {
  // War Helm's Berserk lasts 7 seconds at 60 WIS, the whole window.
  rotmg::Character berserk = kChar;
  berserk.status.berserk = true;
  const auto [per_volley, volleys] = Volleys(berserk, 2);
  const auto b = rotmg::EvaluateBurst(Sword(), Find("War Helm"), WithMp(200), {}, {}, {2, 20});
  CHECK(b.weapon_damage == Approx(volleys * per_volley));
  CHECK(b.ability_damage == 0);
}

TEST_CASE("only the part of a poison that lands in the window counts") {
  // Toxic Vial at 60 WIS: 380 on impact and 1230 over 4.5 seconds, a
  // second of which is in the window.
  const auto b = rotmg::EvaluateBurst(Sword(), Find("Toxic Vial"), WithMp(100), {}, {}, {1, 20});
  CHECK(b.casts == Approx(1));
  CHECK(b.ability_damage == Approx(380 + 1230 / 4.5));
}

TEST_CASE("an on-cast effect in a burst rolls per cast and waits out its own cooldown") {
  // A free ability with a 60% lightning on a cooldown of 8 s: three casts
  // in the window, at 0, 0.45 and 0.9 s. The lightning is ready at the first
  // and, once it goes off, can't again within the window.
  rotmg::Ability ability;
  ability.mp_cost = 0;
  ability.cooldown = 0.45;
  rotmg::Activate lightning;
  lightning.type = "Lightning";
  lightning.params.emplace("totalDamage", 1000.0);
  lightning.params.emplace("proc", 0.6);
  lightning.params.emplace("cooldown", 8.0);
  lightning.params.emplace("maxTargets", 1.0);
  ability.on_ability.push_back(lightning);
  const auto b = rotmg::EvaluateBurst(Sword(), &ability, WithMp(200), {}, {}, {0.9, 10});
  CHECK(b.casts == Approx(3));
  CHECK(b.ability_damage == Approx(1000 * (1 - 0.4 * 0.4 * 0.4)));
}

TEST_CASE("MP given back in a burst window pays toward the next window's casts") {
  // 200 MP fits two 80 MP casts in 3 seconds; 77 MP regenerates in 10
  // seconds. Getting 20 back on each cast makes a cast cost 60, so 77 / 60
  // casts a window.
  rotmg::Ability spell = *Find("Plain Spell");
  rotmg::Activate magic;
  magic.type = "Magic";
  magic.params.emplace("amount", 20.0);
  spell.on_ability.push_back(magic);
  const auto b = rotmg::EvaluateBurst(Sword(), &spell, WithMp(200), {}, {}, {3, 10});
  CHECK(b.window_casts == 2);
  CHECK(b.paid_casts == Approx(77.0 / 60));
  CHECK(b.casts == Approx(77.0 / 60));
}

TEST_CASE("a build's numbers alone match its full evaluation exactly") {
  std::vector<const rotmg::Ability*> abilities = {nullptr};
  for (const rotmg::Ability& a : Data().abilities) abilities.push_back(&a);
  for (const rotmg::DpsWeights& weights : {rotmg::DpsWeights{1, 1}, rotmg::DpsWeights{1, 0}, rotmg::DpsWeights{0, 1},
                                            rotmg::DpsWeights{1, 1, "WIS"}, rotmg::DpsWeights{1, 1, "ATT"}}) {
    for (const rotmg::Weapon& w : Data().weapons) {
      for (const rotmg::Ability* a : abilities) {
        for (const int wisdom : {0, 40, 75}) {
          const rotmg::Character c{.attack = 60, .dexterity = 55, .wisdom = wisdom};
          const rotmg::Target t{.defense = 20};
          const rotmg::BuildBreakdown full = EvaluateBuild(w, a, c, t, {}, weights);
          const rotmg::BuildNumbers numbers = rotmg::EvaluateBuildNumbers(w, a, c, t, {}, weights);
          CHECK(numbers.dps == full.dps);
          CHECK(numbers.weapon_dps == full.weapon_dps);
          CHECK(numbers.ability_dps == (full.ability ? full.ability->dps : 0.0));
          CHECK(numbers.stat_dps == full.stat_dps);
        }
      }
    }
  }
}

TEST_CASE("an ability that weakens its caster is only cast as often as helps") {
  // Like Mirror Cloak: Weak for 7 s on every cast. Casting it costs damage,
  // so the best rhythm is not casting it, and more WIS can't change that.
  rotmg::Ability cloak = *Find("War Helm");
  cloak.activates.clear();
  rotmg::Activate weak;
  weak.type = "ConditionEffectSelf";
  weak.params = {{"effect", std::string("Weak")}, {"duration", 7.0}};
  cloak.activates.push_back(weak);
  const double weapon_alone = EvaluateBuild(Sword(), nullptr, kChar, {}, {}).dps;
  for (int wisdom = 30; wisdom <= 150; wisdom += 10) {
    rotmg::Character c = kChar;
    c.wisdom = wisdom;
    const auto b = EvaluateBuild(Sword(), &cloak, c, {}, {});
    INFO("WIS " << wisdom);
    CHECK(b.dps == Approx(weapon_alone).epsilon(1e-6));
    REQUIRE(b.ability.has_value());
    CHECK(b.ability->casts_per_second < 1e-6);
  }
  // Counting only the ability's own damage, which Weak doesn't touch, the
  // cloak is cast as soon as it can be.
  const auto ability_only = EvaluateBuild(Sword(), &cloak, kChar, {}, {}, {.weapon = 0, .ability = 1});
  REQUIRE(ability_only.ability.has_value());
  CHECK(ability_only.ability->casts_per_second > 0);
  CHECK(ability_only.dps < weapon_alone);
}

TEST_CASE("an effect's damage scales with its scaling stat when a statMod amount adds to it") {
  CHECK(rotmg::DamageScaling(Find("Plain Spell")->activates[0]) == "WIS");
  CHECK(rotmg::DamageScaling(Find("Shock Scepter")->activates[0]) == "WIS");
  // War Helm's aura only lasts longer with WIS.
  CHECK(rotmg::DamageScaling(Find("War Helm")->activates[0]).empty());
  rotmg::Activate shield;
  shield.type = "Shoot";
  shield.params.emplace("scalingStat", std::string("VIT"));
  shield.params.emplace("statModDamage", 3.0);
  CHECK(rotmg::DamageScaling(shield) == "VIT");
}

TEST_CASE("only damage from sources that scale with the stat counts for it") {
  const auto with = [](const char* ability, const char* stat) {
    return EvaluateBuild(Sword(), Find(ability), kChar, {}, {}, {1, 1, stat});
  };
  // Plain Spell's burst scales with WIS; the sword's shots with ATT and DEX.
  const auto wis = with("Plain Spell", "WIS");
  REQUIRE(wis.ability);
  CHECK(wis.stat_dps == Approx(wis.ability->dps));
  const auto att = with("Plain Spell", "ATT");
  CHECK(att.stat_dps == Approx(att.weapon_dps));
  CHECK(with("Plain Spell", "VIT").stat_dps == 0);
  // War Helm deals no damage of its own: its buff raises the sword's.
  CHECK(with("War Helm", "WIS").stat_dps == 0);
  const auto helm = with("War Helm", "DEX");
  CHECK(helm.stat_dps == Approx(helm.weapon_dps));

  const auto burst = [](const char* stat) {
    return rotmg::EvaluateBurst(Sword(), Find("Plain Spell"), WithMp(200), {}, {}, {3, 10}, stat);
  };
  CHECK(burst("WIS").stat_damage == Approx(burst("WIS").ability_damage));
  CHECK(burst("ATT").stat_damage == Approx(burst("ATT").weapon_damage));
  CHECK(burst("VIT").stat_damage == 0);
}
