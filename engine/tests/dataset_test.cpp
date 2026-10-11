#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>

#include "rotmg/dataset.hpp"

namespace {

std::string ReadFixture(const std::string& name) {
  std::ifstream in(std::string(ROTMG_FIXTURES_DIR) + "/" + name, std::ios::binary);
  REQUIRE(in);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

}  // namespace

TEST_CASE("loads the small fixture dataset") {
  const rotmg::Dataset d = rotmg::ParseDataset(ReadFixture("dataset-small.json"));
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
