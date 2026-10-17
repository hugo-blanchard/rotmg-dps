#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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
  const rotmg::Gear* ring = rotmg::FindRing(d, "Plain Ring");
  REQUIRE(ring != nullptr);
  CHECK(ring->type == 28690);
  CHECK(ring->stat_bonuses.size() == 1);
  CHECK(ring->labels == std::vector<std::string>{"EQUIPMENT", "RING", "UT"});
  CHECK(ring->unsupported == std::vector<std::string>{"OnPlayerHitActivate"});
  CHECK(rotmg::FindArmor(d, "Plain Ring") == nullptr);
}

TEST_CASE("loads how items take enchantments and which are reskins") {
  const rotmg::Dataset d = LoadSmallDataset();
  const rotmg::Weapon* sword = rotmg::FindWeapon(d, "Plain Sword");
  CHECK(sword->enchanting.slots == 4);
  CHECK(sword->enchanting.pool == "Default Enchantment Pool");
  CHECK(rotmg::FindWeapon(d, "Burst Bow")->enchanting.slots == 0);
  CHECK(rotmg::FindAbility(d, "War Helm")->labels == std::vector<std::string>{"EQUIPMENT", "ABILITY"});
  CHECK(rotmg::FindRing(d, "Plain Amulet")->enchanting.pool.empty());
  CHECK(rotmg::FindRing(d, "Festive Ring")->reskin_of == "Plain Ring");
  CHECK(rotmg::FindRing(d, "Plain Ring")->reskin_of.empty());
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

TEST_CASE("finds items and enchantments by name, also after the lists change") {
  rotmg::Dataset d = LoadSmallDataset();
  REQUIRE(rotmg::FindArmor(d, "Spark Coat") != nullptr);
  CHECK(rotmg::FindArmor(d, "Spark Coat")->name == "Spark Coat");
  CHECK(rotmg::FindRing(d, "Festive Ring")->name == "Festive Ring");
  CHECK(rotmg::FindRing(d, "Spark Coat") == nullptr);
  CHECK(rotmg::FindEnchantment(d, "Dexterity_Bonus_2")->id == "Dexterity_Bonus_2");
  CHECK(rotmg::FindEnchantment(d, "No_Such_Enchantment") == nullptr);

  rotmg::Weapon copy = *rotmg::FindWeapon(d, "Plain Sword");
  copy.name = "Copied Sword";
  d.weapons.push_back(copy);
  CHECK(rotmg::FindWeapon(d, "Copied Sword") != nullptr);
  CHECK(rotmg::FindWeapon(d, "Plain Sword")->name == "Plain Sword");
}

TEST_CASE("an effect's parameters are found by name or by key, in name order") {
  rotmg::Activate a;
  a.params.emplace("proc", 0.5);
  a.params.emplace("cooldown", 2.0);
  a.params.emplace("cooldown", 9.0);
  a.params["stat"] = std::string("DEX");
  a.params.emplace("someNewParameter", true);
  CHECK(a.Number(rotmg::ParamKey::kCooldown) == 2.0);
  CHECK(a.Number("cooldown") == 2.0);
  CHECK(a.Text(rotmg::ParamKey::kStat) == "DEX");
  CHECK(a.Flag("someNewParameter"));
  CHECK_FALSE(a.Number(rotmg::ParamKey::kDuration).has_value());
  CHECK(rotmg::ParamKeyOf("mustWearOR") == rotmg::ParamKey::kMustWearOR);
  CHECK(rotmg::ParamKeyOf("someNewParameter") == rotmg::ParamKey::kOther);
  std::vector<std::string> names;
  for (const auto& [name, value] : a.params) names.push_back(name);
  CHECK(names == std::vector<std::string>{"cooldown", "proc", "someNewParameter", "stat"});

  a.params = {{"stat", std::string("ATT")}, {"amount", 5.0}};
  CHECK(a.params.size() == 2);
  CHECK(a.Number(rotmg::ParamKey::kAmount) == 5.0);
  CHECK_FALSE(a.Number(rotmg::ParamKey::kProc).has_value());
}

TEST_CASE("an ability with modes is used in its first, with that mode's subattack") {
  nlohmann::json d = nlohmann::json::parse(ReadSmallDatasetText());
  nlohmann::json sigil = d["abilities"][0];
  sigil["name"] = "Two Mode Sigil";
  sigil["type"] = 990;
  sigil.erase("activates");
  sigil.erase("mpCost");
  sigil["subattacks"] = {{{"projectileId", 0}, {"numProjectiles", 1}, {"rateOfFire", 1}},
                         {{"projectileId", 0}, {"numProjectiles", 2}, {"rateOfFire", 1}}};
  sigil["modes"] = {{{"name", "Normal Ability"}, {"attackIndex", 0}, {"mpCost", 100},
                     {"activates", {{{"type", "Shoot"}}}}},
                    {{"name", "Form"}, {"attackIndex", 1}, {"mpCost", 100}, {"abilityTime", 9.6},
                     {"activates", {{{"type", "Shoot"}}, {{"type", "BulletCreate"}}}}}};
  sigil["meter"] = {{"id", 0}, {"maxValue", 500}};
  d["abilities"].push_back(sigil);
  const rotmg::Dataset data = rotmg::ParseDataset(d.dump());
  const rotmg::Ability* a = rotmg::FindAbility(data, "Two Mode Sigil");
  REQUIRE(a != nullptr);
  REQUIRE(a->modes.size() == 2);
  CHECK(a->modes[1].ability_time == 9.6);
  CHECK(a->meter->max_value == 500);
  REQUIRE(a->activates.size() == 1);
  CHECK(a->activates[0].type == "Shoot");
  CHECK(a->mp_cost == 100);
  REQUIRE(a->subattacks.size() == 1);
  CHECK(a->subattacks[0].num_projectiles == 1);
  CHECK(a->mode_subattacks.size() == 2);
}
