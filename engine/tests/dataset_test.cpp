#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "fixture.hpp"
#include "rotmg/dataset.hpp"

TEST_CASE("loads the small fixture dataset") {
  const rotmg::Dataset d = LoadSmallDataset();
  CHECK(d.build == "fixture");
  REQUIRE(d.classes.size() == 2);
  CHECK(d.classes[1].id == "Bowman");
  CHECK(d.classes[1].weapon_slot_type == 3);
  CHECK(d.classes[1].dexterity.max == 75);
  REQUIRE(d.weapons.size() == 4);
  REQUIRE(d.sets.size() == 1);
  CHECK(d.sets[0].key_pieces == std::vector<int>{4097, 20482, 28689, 28690});

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

TEST_CASE("rejects a flat stat amount that isn't whole tenths") {
  std::string text = ReadSmallDatasetText();
  const std::string tenths = R"("stat": "ATT", "amount": 1.4 })";
  REQUIRE(text.find(tenths) != std::string::npos);
  text.replace(text.find(tenths), tenths.size(), R"("stat": "ATT", "amount": 1.45 })");
  CHECK_THROWS_AS(rotmg::ParseDataset(text), rotmg::DatasetError);
}

TEST_CASE("loads abilities and the rest of the class stats") {
  const rotmg::Dataset d = LoadSmallDataset();
  CHECK(d.classes[0].ability_slot_type == 10);
  CHECK(d.classes[0].wisdom.max == 60);
  REQUIRE(d.abilities.size() == 12);

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

TEST_CASE("loads armor, rings and the slots classes wear them in") {
  const rotmg::Dataset d = LoadSmallDataset();
  CHECK(d.classes[0].armor_slot_type == 7);
  CHECK(d.classes[0].ring_slot_type == 9);
  REQUIRE(d.armors.size() == 2);
  REQUIRE(d.rings.size() == 3);
  const rotmg::Gear* ring = rotmg::FindGear(d.rings, "Plain Ring");
  REQUIRE(ring != nullptr);
  CHECK(ring->type == 28690);
  CHECK(ring->stat_bonuses.size() == 1);
  CHECK(ring->labels == std::vector<std::string>{"EQUIPMENT", "RING", "UT"});
  CHECK(ring->unsupported == std::vector<std::string>{"OnPlayerHitActivate"});
  CHECK(rotmg::FindGear(d.armors, "Plain Ring") == nullptr);
}

TEST_CASE("loads how items take enchantments and which are reskins") {
  const rotmg::Dataset d = LoadSmallDataset();
  const rotmg::Weapon* sword = rotmg::FindWeapon(d, "Plain Sword");
  CHECK(sword->enchanting.slots == 4);
  CHECK(sword->enchanting.pool == "Default Enchantment Pool");
  CHECK(rotmg::FindWeapon(d, "Burst Bow")->enchanting.slots == 0);
  CHECK(rotmg::FindAbility(d, "War Helm")->labels == std::vector<std::string>{"EQUIPMENT", "ABILITY"});
  CHECK(rotmg::FindGear(d.rings, "Plain Amulet")->enchanting.pool.empty());
  CHECK(rotmg::FindGear(d.rings, "Festive Ring")->reskin_of == "Plain Ring");
  CHECK(rotmg::FindGear(d.rings, "Plain Ring")->reskin_of.empty());
  CHECK(rotmg::FindAbility(d, "Relic Helm")->unobtainable);
  CHECK_FALSE(rotmg::FindAbility(d, "War Helm")->unobtainable);
  REQUIRE(d.enchantment_pools.size() == 1);
  CHECK(d.enchantment_pools.at("Default Enchantment Pool").contains("Damage_Bonus_4"));
}

TEST_CASE("a dataset without abilities still loads") {
  const rotmg::Dataset d = rotmg::ParseDataset(R"({"schemaVersion": 1, "build": "old",
    "classes": [{"type": 1, "id": "Old", "weaponSlotType": 1, "attack": {"base": 1, "max": 2}, "dexterity": {"base": 1, "max": 2}}],
    "weapons": []})");
  CHECK(d.abilities.empty());
  CHECK(d.classes[0].ability_slot_type == 0);
  CHECK(d.classes[0].wisdom.max == 0);
}
