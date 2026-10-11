#include <catch2/catch_test_macros.hpp>

#include <string>

#include "fixture.hpp"
#include "rotmg/dataset.hpp"

TEST_CASE("loads the small fixture dataset") {
  const rotmg::Dataset d = LoadSmallDataset();
  CHECK(d.build == "fixture");
  REQUIRE(d.classes.size() == 2);
  CHECK(d.classes[1].id == "Bowman");
  CHECK(d.classes[1].weapon_slot_type == 3);
  CHECK(d.classes[1].dexterity.max == 75);
  REQUIRE(d.weapons.size() == 3);

  const rotmg::Weapon* sword = rotmg::FindWeapon(d, "Plain Sword");
  REQUIRE(sword != nullptr);
  CHECK(sword->tier == 5);
  CHECK(sword->StatBonusFor("DEX") == 4);
  CHECK(sword->StatBonusFor("ATT") == 0);
  CHECK(sword->subattacks[0].rate_of_fire == 1);
  CHECK_FALSE(sword->subattacks[0].burst_count.has_value());

  const rotmg::Weapon* bow = rotmg::FindWeapon(d, "Burst Bow");
  REQUIRE(bow != nullptr);
  CHECK_FALSE(bow->tier.has_value());
  const rotmg::Projectile* piercing = bow->FindProjectile(1);
  REQUIRE(piercing != nullptr);
  CHECK(piercing->armor_piercing);
  CHECK(piercing->condition_effects.size() == 1);
  CHECK(bow->subattacks[0].pos_offset == std::array<double, 2>{-0.5, 0});
  CHECK(bow->subattacks[0].burst_count == 4);
  CHECK(bow->FindProjectile(7) == nullptr);

  CHECK(rotmg::FindWeapon(d, "Proc Wand")->unsupported == std::vector<std::string>{"OnPlayerShootActivate"});
  CHECK(rotmg::FindWeapon(d, "No Such Weapon") == nullptr);
}

TEST_CASE("rejects other schema versions") {
  CHECK_THROWS_AS(rotmg::ParseDataset(R"({"schemaVersion": 2, "build": "x", "classes": [], "weapons": []})"),
                  rotmg::DatasetError);
}

TEST_CASE("rejects malformed JSON and missing fields") {
  CHECK_THROWS_AS(rotmg::ParseDataset("{"), rotmg::DatasetError);
  CHECK_THROWS_AS(rotmg::ParseDataset(R"({"schemaVersion": 1, "build": "x", "classes": []})"), rotmg::DatasetError);
}

TEST_CASE("loads abilities and the rest of the class stats") {
  const rotmg::Dataset d = LoadSmallDataset();
  CHECK(d.classes[0].ability_slot_type == 10);
  CHECK(d.classes[0].wisdom.max == 60);
  REQUIRE(d.abilities.size() == 11);

  const rotmg::Ability* spell = rotmg::FindAbility(d, "Plain Spell");
  REQUIRE(spell != nullptr);
  CHECK(spell->mp_cost == 80);
  CHECK_FALSE(spell->cooldown.has_value());
  REQUIRE(spell->activates.size() == 1);
  const rotmg::Activate& nova = spell->activates[0];
  CHECK(nova.type == "BulletNova");
  CHECK(nova.Number("numShots") == 16);
  CHECK(nova.Text("scalingStat") == "WIS");
  CHECK_FALSE(nova.Number("scalingStat").has_value());
  CHECK(nova.Text("missing").empty());
  CHECK(spell->AsWeapon().subattacks.size() == 1);

  CHECK(rotmg::FindAbility(d, "Odd Cloak")->unsupported == std::vector<std::string>{"MpCostPerSecond"});
  CHECK(rotmg::FindAbility(d, "No Such Ability") == nullptr);
}

TEST_CASE("a dataset without abilities still loads") {
  const rotmg::Dataset d = rotmg::ParseDataset(R"({"schemaVersion": 1, "build": "old",
    "classes": [{"type": 1, "id": "Old", "weaponSlotType": 1, "attack": {"base": 1, "max": 2}, "dexterity": {"base": 1, "max": 2}}],
    "weapons": []})");
  CHECK(d.abilities.empty());
  CHECK(d.classes[0].ability_slot_type == 0);
  CHECK(d.classes[0].wisdom.max == 0);
}
