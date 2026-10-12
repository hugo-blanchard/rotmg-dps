#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "fixture.hpp"
#include "rotmg/ability.hpp"

using Catch::Approx;
using rotmg::Buff;
using rotmg::EvaluateAbility;

namespace {

const rotmg::Dataset& Data() {
  static const rotmg::Dataset dataset = LoadSmallDataset();
  return dataset;
}

const rotmg::Ability& Ability(const char* name) {
  const rotmg::Ability* a = rotmg::FindAbility(Data(), name);
  REQUIRE(a != nullptr);
  return *a;
}

rotmg::Activate Effect(std::initializer_list<std::pair<const char*, rotmg::Activate::Value>> params) {
  rotmg::Activate a;
  for (const auto& [key, value] : params) {
    a.params.emplace(key, value);
  }
  return a;
}

const rotmg::Character kWis60{.attack = 50, .dexterity = 50, .wisdom = 60};

bool Has(const std::vector<std::string>& list, const std::string& item) {
  return std::ranges::find(list, item) != list.end();
}

}  // namespace

TEST_CASE("Character Stats: MP regen is 6.5/s at 50 WIS and 9.5/s at 75") {
  CHECK(rotmg::MpPerSecond(50) == Approx(6.5));
  CHECK(rotmg::MpPerSecond(75) == Approx(9.5));
}

TEST_CASE("Character Stats: Burning Retribution gains 2.25 damage per WIS over 42") {
  const auto spell = Effect({{"scalingStat", std::string("WIS")}, {"statModScalingMin", 42.0}, {"statModDamage", 2.25}});
  CHECK(rotmg::StatModBonus(spell, "statModDamage", {.wisdom = 60}) == Approx(40.5));
  CHECK(rotmg::StatModBonus(spell, "statModDamage", {.wisdom = 30}) == 0);
}

TEST_CASE("Character Stats: one more shot per 15 WIS over 42, in whole steps") {
  const auto spell =
      Effect({{"scalingStat", std::string("WIS")}, {"statModScalingMin", 42.0}, {"statModNumShots", 1.0 / 15}});
  CHECK(rotmg::StatModCount(spell, "statModNumShots", {.wisdom = 56}) == 0);
  CHECK(rotmg::StatModCount(spell, "statModNumShots", {.wisdom = 57}) == 1);
  CHECK(rotmg::StatModCount(spell, "statModNumShots", {.wisdom = 71}) == 1);
  CHECK(rotmg::StatModCount(spell, "statModNumShots", {.wisdom = 72}) == 2);
}

TEST_CASE("Helms: Helm of the Great General's Berserk lasts 4s, 6s at 50 WIS") {
  rotmg::Ability helm;
  helm.activates.push_back(Effect({{"effect", std::string("Berserk")},
                                   {"duration", 4.0},
                                   {"scalingStat", std::string("WIS")},
                                   {"statModScalingMin", 30.0},
                                   {"statModDuration", 0.1}}));
  helm.activates[0].type = "ConditionEffectAura";
  CHECK(EvaluateAbility(helm, {.wisdom = 30}, {}, {}).buffs[0].duration_s == Approx(4));
  CHECK(EvaluateAbility(helm, {.wisdom = 50}, {}, {}).buffs[0].duration_s == Approx(6));
}

TEST_CASE("Seals: Damaging lasts 4s, 6.5s at 75 WIS") {
  rotmg::Ability seal;
  seal.activates.push_back(Effect({{"effect", std::string("Damaging")},
                                   {"duration", 4.0},
                                   {"scalingStat", std::string("WIS")},
                                   {"statModScalingMin", 50.0},
                                   {"statModDuration", 0.1}}));
  seal.activates[0].type = "ConditionEffectAura";
  CHECK(EvaluateAbility(seal, {.wisdom = 75}, {}, {}).buffs[0].duration_s == Approx(6.5));
}

TEST_CASE("a spell lands every shot, with StatMod damage and shots") {
  // 60 WIS: +36 damage per shot, +1 shot. Rolls 95-199 average 147.
  const auto b = EvaluateAbility(Ability("Plain Spell"), kWis60, {}, {});
  REQUIRE(b.parts.size() == 1);
  CHECK(b.parts[0].damage == Approx(17 * (147 + 36)));
  CHECK(b.mp_per_second == Approx(7.7));
  CHECK(b.casts_per_second == Approx(7.7 / 80));
  CHECK(b.dps == Approx(17 * 183 * 7.7 / 80));
  CHECK(b.cooldown_s == 0.5);
  CHECK(Has(b.assumptions, "an ability with no cooldown set can be used every 0.5 seconds"));
}

TEST_CASE("DEF applies to each spell shot") {
  const auto open = EvaluateAbility(Ability("Plain Spell"), kWis60, {}, {});
  const auto armored = EvaluateAbility(Ability("Plain Spell"), kWis60, {.defense = 50}, {});
  CHECK(armored.damage_per_cast == Approx(open.damage_per_cast - 17 * 50));
}

TEST_CASE("buffs: Berserk from a helm, Damaging and ATT from a seal") {
  const auto helm = EvaluateAbility(Ability("War Helm"), kWis60, {}, {});
  REQUIRE(helm.buffs.size() == 1);  // Speedy doesn't change DPS
  CHECK(helm.buffs[0].effect == "Berserk");
  CHECK(helm.buffs[0].duration_s == Approx(7));
  CHECK(helm.dps == 0);

  // The seal's own +4 WIS counts toward its StatMod: 64 WIS is 14 over 50.
  const auto seal = EvaluateAbility(Ability("Battle Seal"), kWis60, {}, {});
  REQUIRE(seal.buffs.size() == 2);
  CHECK(seal.buffs[0].effect == "Damaging");
  CHECK(seal.buffs[0].duration_s == Approx(5.4));
  CHECK(seal.buffs[1].effect == "ATT");
  CHECK(seal.buffs[1].amount == 10);
  CHECK(seal.mp_per_second == Approx(rotmg::MpPerSecond(64)));
}

TEST_CASE("an orb curses the target, longer with more WIS") {
  const auto b = EvaluateAbility(Ability("Curse Orb"), kWis60, {}, {});
  REQUIRE(b.buffs.size() == 1);
  CHECK(b.buffs[0].on == Buff::On::kTarget);
  CHECK(b.buffs[0].effect == "Curse");
  CHECK(b.buffs[0].duration_s == Approx(4.5));
}

TEST_CASE("a wakizashi's slash lands every shot and exposes the target") {
  // 18 WIS over 42: 0.06 * 18 = 1.08 adds one shot, and 3 per point adds 54
  // damage to each. Rolls 350-449 average 399.5.
  const auto b = EvaluateAbility(Ability("Plain Wakizashi"), kWis60, {}, {});
  REQUIRE(b.parts.size() == 2);
  CHECK(b.parts[0].name == "Shots at the cursor, 4");
  CHECK(b.parts[0].damage == Approx(4 * (399.5 + 54)));
  REQUIRE(b.buffs.size() == 1);
  CHECK(b.buffs[0].effect == "Exposed");
  CHECK(b.buffs[0].duration_s == Approx(2));
  // The second activate makes item 0x6008's projectile, 100-199 with no
  // scaling; the third names an item the dataset doesn't have.
  CHECK(b.parts[1].damage == Approx(149.5));
  CHECK(Has(b.unsupported, "BulletCreate shots of another projectile"));
  CHECK_FALSE(Has(b.unsupported, "BulletCreate"));
}

TEST_CASE("a quiver's shots use the hit estimate and expose the target") {
  // 4 shots 7 degrees apart: the outer pair is 10.5 degrees off center, so
  // it passes 2.5 * sin(10.5) = 0.46 tiles from a target 2.5 tiles away (a
  // hit) and 0.55 from one 3 tiles away (a miss). Rolls 295-334 average
  // 314.5, and 26 WIS over 34 adds 3.88 each.
  const double per_hit = 314.5 + 3.88 * 26;
  rotmg::Assumptions a;
  a.distance = 2.5;
  const auto close = EvaluateAbility(Ability("Plain Quiver"), kWis60, {}, a);
  REQUIRE(close.parts.size() == 1);
  CHECK(close.parts[0].damage == Approx(4 * per_hit));
  REQUIRE(close.buffs.size() == 1);
  CHECK(close.buffs[0].effect == "Exposed");

  a.distance = 3;
  CHECK(EvaluateAbility(Ability("Plain Quiver"), kWis60, {}, a).parts[0].damage == Approx(2 * per_hit));
}

TEST_CASE("poison ignores DEF and flags its damage boost") {
  // 60 WIS: impact 350 + 30, over time 1050 + 180.
  const auto open = EvaluateAbility(Ability("Toxic Vial"), kWis60, {}, {});
  const auto armored = EvaluateAbility(Ability("Toxic Vial"), kWis60, {.defense = 100}, {});
  CHECK(open.damage_per_cast == Approx(1610));
  CHECK(armored.damage_per_cast == Approx(1610));
  CHECK(Has(open.unsupported, "poison's damage boost to poisoned targets"));
}

TEST_CASE("lightning, traps and damage novas") {
  // Scepter of Storms' numbers: 8 more damage per WIS over 50 (wisDamageBase
  // 80 / 10), as RealmEye's Scepters table lists.
  const auto lightning = EvaluateAbility(Ability("Shock Scepter"), kWis60, {}, {});
  CHECK(lightning.damage_per_cast == Approx(300 + 8 * 10));
  CHECK(lightning.unsupported.empty());

  const auto trap = EvaluateAbility(Ability("Snare Trap"), kWis60, {.defense = 40}, {});
  CHECK(trap.damage_per_cast == Approx(460 + 5 * 26 - 40));
  CHECK(trap.cooldown_s == 1);
  CHECK(trap.casts_per_second == Approx(7.7 / 105));

  // Below the tome's 70 WIS threshold, so no bonus: two blasts of 600-680.
  const auto tome = EvaluateAbility(Ability("Holy Tome"), kWis60, {}, {});
  CHECK(tome.damage_per_cast == Approx(2 * 640));
  CHECK(tome.unsupported.empty());
}

TEST_CASE("effects that aren't modelled are listed, harmless ones aren't") {
  const auto b = EvaluateAbility(Ability("Odd Cloak"), kWis60, {}, {});
  CHECK(Has(b.unsupported, "MpCostPerSecond"));
  CHECK(Has(b.unsupported, "SpawnCreep"));
  CHECK_FALSE(Has(b.unsupported, "Sneak"));
  CHECK_FALSE(Has(b.unsupported, "DamageMultAura"));
  CHECK(b.casts_per_second == Approx(7.7 / 100));
}

TEST_CASE("Scepters: lightning and shockblast scale as the wiki's tables say") {
  // Scepter of Storms at 75 WIS: 300 + 8 * 25 damage, 5 + 2 targets; its
  // shockblast 75 + 2.2 * 25 on 2 + 2 targets.
  const rotmg::Activate& storms = Ability("Shock Scepter").activates[0];
  const rotmg::Character c{.wisdom = 75};
  CHECK(rotmg::LightningDamage(storms, c) == Approx(500));
  CHECK(rotmg::LightningTargets(storms, c) == 7);
  CHECK(rotmg::ShockblastDamage(storms, c) == Approx(130));
  CHECK(rotmg::ShockblastTargets(storms, c) == 4);
  // Nothing extra at or under 50 WIS.
  CHECK(rotmg::LightningDamage(storms, {.wisdom = 40}) == Approx(300));
  CHECK(rotmg::LightningTargets(storms, {.wisdom = 50}) == 5);
}
