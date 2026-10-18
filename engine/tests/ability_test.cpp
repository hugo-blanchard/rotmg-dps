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

TEST_CASE("Pet Abilities: a level 100 Magic Heal restores 45 MP every second") {
  CHECK(rotmg::MagicHealMpPerSecond(100) == Approx(45));
  CHECK(rotmg::MagicHealMpPerSecond(90) == Approx(33 / 1.42));
  CHECK(rotmg::MagicHealMpPerSecond(30) == Approx(3 / 3.82));
  CHECK(rotmg::MagicHealMpPerSecond(1) == Approx(0.1));
  CHECK(rotmg::MagicHealMpPerSecond(0) == 0);
  // Between the ends of a rarity's levels, halfway along both.
  CHECK(rotmg::MagicHealMpPerSecond(95) == Approx(39 / 1.21));
}

TEST_CASE("Character Stats: Burning Retribution gains 2.25 damage per WIS over 42") {
  const auto spell = Effect({{"scalingStat", std::string("WIS")}, {"statModScalingMin", 42.0}, {"statModDamage", 2.25}});
  CHECK(rotmg::StatModBonus(spell, rotmg::ParamKey::kStatModDamage, {.wisdom = 60}) == Approx(40.5));
  CHECK(rotmg::StatModBonus(spell, rotmg::ParamKey::kStatModDamage, {.wisdom = 30}) == 0);
}

TEST_CASE("Character Stats: one more shot per 15 WIS over 42, in whole steps") {
  const auto spell =
      Effect({{"scalingStat", std::string("WIS")}, {"statModScalingMin", 42.0}, {"statModNumShots", 1.0 / 15}});
  CHECK(rotmg::StatModCount(spell, rotmg::ParamKey::kStatModNumShots, {.wisdom = 56}) == 0);
  CHECK(rotmg::StatModCount(spell, rotmg::ParamKey::kStatModNumShots, {.wisdom = 57}) == 1);
  CHECK(rotmg::StatModCount(spell, rotmg::ParamKey::kStatModNumShots, {.wisdom = 71}) == 1);
  CHECK(rotmg::StatModCount(spell, rotmg::ParamKey::kStatModNumShots, {.wisdom = 72}) == 2);
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
  CHECK(b.cooldown_s == 0.45);
  CHECK(Has(b.assumptions,
            "an ability with no cooldown set can be used every 0.45 seconds, the shortest any ability allows"));
}

TEST_CASE("no ability can be used more often than every 0.45 seconds") {
  rotmg::Ability quick;
  quick.cooldown = 0.2;
  CHECK(rotmg::AbilityCooldown(quick) == 0.45);
  quick.cooldown = 3;
  CHECK(rotmg::AbilityCooldown(quick) == 3);
  CHECK(EvaluateAbility(quick, kWis60, {}, {}).casts_per_second == Approx(1 / 3.0));
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

TEST_CASE("poison ignores DEF and boosts damage to what it poisons") {
  // 60 WIS, 10 over 50: impact 350 + 3 a point, over time 1050 + 18 a
  // point (StatMods of 30 and 180 count per 10 points), and a damage boost
  // of 9.5% + 0.225% * 10 for the poison's 4.5 seconds.
  const auto open = EvaluateAbility(Ability("Toxic Vial"), kWis60, {}, {});
  const auto armored = EvaluateAbility(Ability("Toxic Vial"), kWis60, {.defense = 100}, {});
  CHECK(open.damage_per_cast == Approx(1610));
  CHECK(armored.damage_per_cast == Approx(1610));
  // The impact lands at once and the rest over the poison's duration.
  REQUIRE(open.parts.size() == 2);
  CHECK(open.parts[0].damage == Approx(380));
  CHECK(open.parts[0].over_s == 0);
  CHECK(open.parts[1].damage == Approx(1230));
  CHECK(open.parts[1].over_s == Approx(4.5));
  REQUIRE(open.buffs.size() == 1);
  CHECK(open.buffs[0].effect == rotmg::kPoisonBoost);
  CHECK(open.buffs[0].on == Buff::On::kTarget);
  CHECK(open.buffs[0].factor == Approx(1.1175));
  CHECK(open.buffs[0].duration_s == Approx(4.5));
  CHECK(open.unsupported.empty());
}

TEST_CASE("lightning, traps and damage novas") {
  // Scepter of Storms' numbers: 8 more damage per WIS over 50 (wisDamageBase
  // 80 / 10), as RealmEye's Scepters table lists.
  const auto lightning = EvaluateAbility(Ability("Shock Scepter"), kWis60, {}, {});
  CHECK(lightning.damage_per_cast == Approx(300 + 8 * 10));
  CHECK(lightning.unsupported.empty());

  // 26 WIS over 34: the trap 460 + 5 * 26 and its sticky bomb 715 + 5 * 26,
  // each less 40 DEF.
  const auto trap = EvaluateAbility(Ability("Snare Trap"), kWis60, {.defense = 40}, {});
  CHECK(trap.damage_per_cast == Approx(460 + 130 - 40 + 715 + 130 - 40));
  CHECK(trap.unsupported.empty());
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

TEST_CASE("MP given back on casting lowers what a cast costs") {
  // Plain Spell costs 80 MP; at 60 WIS, 7.7 MP/s. Getting 20 back on every
  // cast makes it 60, and half the time makes it 70.
  rotmg::Ability spell = Ability("Plain Spell");
  rotmg::Activate magic;
  magic.type = "Magic";
  magic.params.emplace("amount", 20.0);
  spell.on_ability.push_back(magic);
  const auto every = EvaluateAbility(spell, kWis60, {}, {});
  CHECK(every.mp_refund_per_cast == Approx(20));
  CHECK(every.casts_per_second == Approx(7.7 / 60));
  CHECK(every.unsupported.empty());

  spell.on_ability.back().params.insert_or_assign("proc", 0.5);
  CHECK(EvaluateAbility(spell, kWis60, {}, {}).casts_per_second == Approx(7.7 / 70));
}

TEST_CASE("more WIS never means fewer casts with MP given back on casting") {
  // A refund with a cooldown comes back on fewer casts the more often you
  // cast, which once made the cast rate flip between two values.
  rotmg::Ability spell = Ability("Plain Spell");
  rotmg::Activate magic;
  magic.type = "Magic";
  spell.on_ability.push_back(magic);
  for (const double amount : {20.0, 40.0, 60.0, 79.0}) {
    for (const double cooldown : {0.5, 1.0, 2.0, 3.5}) {
      spell.on_ability.back().params.insert_or_assign("amount", amount);
      spell.on_ability.back().params.insert_or_assign("cooldown", cooldown);
      double previous = 0;
      for (int wis = 0; wis <= 150; ++wis) {
        const double casts = EvaluateAbility(spell, {.wisdom = wis}, {}, {}).casts_per_second;
        INFO(amount << " MP every " << cooldown << " s, WIS " << wis);
        CHECK(casts >= previous - 1e-12);
        previous = casts;
      }
    }
  }
}

namespace {

// A skull that raises one undead firing `shots` of 100 armor-piercing damage
// twice a second for 5 seconds, with an optional status on its shots.
rotmg::Ability Skull(int shots, const char* status = nullptr) {
  rotmg::Ability skull;
  skull.name = "Test Skull";
  skull.mp_cost = 100;
  skull.activates.push_back({"RaiseDead", {{"numDead", 1.0}, {"undeadId", std::string("Test Undead")}}});
  rotmg::Projectile p;
  p.min_damage = 100;
  p.max_damage = 101;
  p.armor_piercing = true;
  if (status != nullptr) {
    p.condition_effects.push_back({status, 2});
  }
  rotmg::Undead u;
  u.projectiles = {p};
  u.shots = shots;
  u.attacks_per_second = 2;
  u.lifetime_s = 5;
  skull.undead.emplace("Test Undead", u);
  return skull;
}

}  // namespace

TEST_CASE("a raised undead attacks at its rate for its lifetime") {
  // 2 attacks a second for 5 s, 2 shots each: 20 shots of 100, through DEF.
  const auto skull = EvaluateAbility(Skull(2), kWis60, {.defense = 50}, {});
  REQUIRE(skull.parts.size() == 1);
  CHECK(skull.parts[0].damage == Approx(2000));
  CHECK(skull.parts[0].over_s == 5);
  CHECK(skull.unsupported.empty());

  // One that curses: every attack after the first lands on a cursed target.
  const auto cursing = EvaluateAbility(Skull(2, "Curse"), kWis60, {}, {});
  CHECK(cursing.parts[0].damage == Approx(2 * 100 + 9 * 2 * 125));
  REQUIRE(cursing.buffs.size() == 1);
  CHECK(cursing.buffs[0].effect == "Curse");
  CHECK(cursing.buffs[0].duration_s == Approx(7));

  // Only whole attacks count: at 1.5 a second, 7 in 5 seconds.
  rotmg::Ability slower = Skull(1);
  slower.undead.at("Test Undead").attacks_per_second = 1.5;
  CHECK(EvaluateAbility(slower, kWis60, {}, {}).parts[0].damage == Approx(700));

  // Like the Baby Kaiju: each attack fires its shots of every projectile.
  rotmg::Ability two = Skull(1);
  rotmg::Projectile blast = two.undead.at("Test Undead").projectiles[0];
  blast.min_damage = 300;
  blast.max_damage = 301;
  two.undead.at("Test Undead").projectiles.push_back(blast);
  CHECK(EvaluateAbility(two, kWis60, {}, {}).parts[0].damage == Approx(10 * (100 + 300)));

  // Like the Brain Cube: bursts over its lifetime, the k-th from the k-th
  // projectile. Through DEF unless they pierce armor.
  rotmg::Ability cube = two;
  rotmg::Undead& bursting = cube.undead.at("Test Undead");
  bursting.bursts = 4;
  bursting.burst_radii = {2.25, 3.25};
  bursting.projectiles[1].armor_piercing = false;
  const auto bursts = EvaluateAbility(cube, kWis60, {.defense = 50}, {});
  CHECK(bursts.parts[0].damage == Approx(2 * 100 + 2 * 250));
  CHECK(bursts.parts[0].over_s == 5);

  // An undead the dataset doesn't describe isn't guessed at.
  rotmg::Ability unknown = Skull(1);
  unknown.undead.clear();
  const auto none = EvaluateAbility(unknown, kWis60, {}, {});
  CHECK(none.dps == 0);
  CHECK(Has(none.unsupported, "RaiseDead"));
}
