#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "fixture.hpp"
#include "rotmg/api.hpp"

using nlohmann::json;

namespace {

rotmg::Api LoadedApi() {
  rotmg::Api api;
  REQUIRE(api.Load(ReadSmallDatasetText()).empty());
  return api;
}

}  // namespace

TEST_CASE("evaluates a request and returns a breakdown and a curve") {
  const rotmg::Api api = LoadedApi();
  const json res = json::parse(api.Evaluate(R"({
    "weapon": "Plain Sword", "attack": 25, "dexterity": 0,
    "target": {"defense": 10},
    "curve": {"from": 0, "to": 20, "step": 10}
  })"));
  REQUIRE(res["ok"] == true);
  const json& b = res["breakdown"];
  CHECK(b["subattacks"].size() == 1);
  CHECK(b["terms"][0]["source"] == "client-data");
  CHECK(b["assumptions"].size() == 2);
  REQUIRE(res["curve"].size() == 3);
  CHECK(res["curve"][1]["defense"] == 10);
  CHECK(res["curve"][1]["dps"].get<double>() == Catch::Approx(b["dps"].get<double>()));
  CHECK(res["curve"][0]["dps"].get<double>() > res["curve"][2]["dps"].get<double>());
}

TEST_CASE("passes statuses and hit overrides through") {
  const rotmg::Api api = LoadedApi();
  const json weak = json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "attack": 75, "status": {"weak": true}})"));
  const json strong = json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "attack": 75})"));
  CHECK(weak["breakdown"]["dps"].get<double>() < strong["breakdown"]["dps"].get<double>());

  const json hits = json::parse(api.Evaluate(R"({"weapon": "Burst Bow", "hitsPerVolley": [1, null]})"));
  CHECK(hits["breakdown"]["subattacks"][0]["hitsPerVolley"] == 1);
  CHECK(hits["breakdown"]["subattacks"][1]["hitsPerVolley"] == 1);
}

TEST_CASE("reports errors instead of throwing") {
  rotmg::Api empty;
  CHECK(json::parse(empty.Evaluate(R"({"weapon": "Plain Sword"})"))["error"] == "no dataset loaded");
  CHECK_FALSE(empty.Load("{").empty());

  const rotmg::Api api = LoadedApi();
  CHECK(json::parse(api.Evaluate(R"({"weapon": "Nope"})"))["ok"] == false);
  CHECK(json::parse(api.Evaluate("not json"))["ok"] == false);
  CHECK(json::parse(api.Evaluate(R"({"attack": 50})"))["ok"] == false);
  CHECK(json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "curve": {"step": 0}})"))["error"] == "bad curve range");
}

TEST_CASE("returns the hit estimate over distance") {
  const rotmg::Api api = LoadedApi();
  const json res = json::parse(api.Evaluate(R"({
    "weapon": "Burst Bow", "hitModel": {"distance": 3},
    "hitCurve": {"from": 1, "to": 9, "step": 1}
  })"));
  REQUIRE(res["ok"] == true);
  REQUIRE(res["hitCurve"].size() == 9);
  CHECK(res["hitCurve"][0]["distance"] == 1.0);
  CHECK(res["hitCurve"][8]["hits"] == 0);  // past the 7 tile range
  CHECK(res["hitModelGaps"].empty());
  CHECK(res["breakdown"]["subattacks"][0]["hitsSource"] == "geometric-estimate");
  CHECK(res["breakdown"]["subattacks"][0]["projectilesPerVolley"] == 2);
}

TEST_CASE("runs a simulation and reports its state") {
  rotmg::Api api = LoadedApi();
  CHECK(json::parse(api.AdvanceSimulation(10))["error"] == "no simulation running");
  REQUIRE(api.StartSimulation(R"({"weapon": "Burst Bow", "attack": 50, "dexterity": 75, "hitModel": {"distance": 3}})")
              .empty());
  const json state = json::parse(api.AdvanceSimulation(1000));
  REQUIRE(state["ok"] == true);
  CHECK(state["timeMs"] == 1000.0);
  CHECK(state["target"]["y"] == 3.0);
  CHECK(state["perSubattack"].size() == 2);
  CHECK(state["totals"]["volleys"].get<int>() > 0);
  CHECK(state["totals"]["hits"].get<int>() > 0);
  CHECK(state["shots"].is_array());

  CHECK_FALSE(api.StartSimulation(R"({"weapon": "Nope"})").empty());
  CHECK(json::parse(api.AdvanceSimulation(10))["ok"] == false);
}

TEST_CASE("evaluates a build with an ability") {
  const rotmg::Api api = LoadedApi();
  const json res = json::parse(api.Evaluate(R"({
    "weapon": "Plain Sword", "ability": "War Helm", "attack": 50, "dexterity": 50, "wisdom": 60,
    "curve": {"from": 0, "to": 10, "step": 10}
  })"));
  REQUIRE(res["ok"] == true);
  const double build = res["build"]["dps"].get<double>();
  CHECK(build > res["breakdown"]["dps"].get<double>());
  CHECK(res["build"]["uptimes"][0]["effect"] == "Berserk");
  CHECK(res["build"]["uptimes"][0]["on"] == "player");
  CHECK(res["ability"]["buffs"][0]["durationS"].get<double>() == Catch::Approx(7));
  CHECK(res["ability"]["castsPerSecond"].get<double>() == Catch::Approx(7.7 / 90));
  CHECK(res["curve"][0]["dps"].get<double>() == Catch::Approx(build));

  const json none = json::parse(api.Evaluate(R"({"weapon": "Plain Sword"})"));
  CHECK_FALSE(none.contains("ability"));
  CHECK(none["build"]["dps"] == none["breakdown"]["dps"]);

  CHECK(json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "ability": "Nope"})"))["error"] ==
        "no ability named \"Nope\"");
}

TEST_CASE("adds set bonuses for the pieces worn") {
  // Swordsman Set: +5 ATT at 2 pieces, +5 at 3, +10 ATT and +4 DEX at 4.
  const rotmg::Api api = LoadedApi();
  const auto attack = [&](const char* request) {
    const json res = json::parse(api.Evaluate(request));
    REQUIRE(res["ok"] == true);
    for (const json& t : res["breakdown"]["terms"]) {
      if (t["name"] == "ATT with weapon") {
        return std::pair{t["value"].get<double>(), res["sets"]};
      }
    }
    FAIL("no ATT term");
    return std::pair{0.0, json()};
  };
  const auto [alone, alone_sets] = attack(R"({"weapon": "Plain Sword", "attack": 50})");
  CHECK(alone == 50);
  REQUIRE(alone_sets.size() == 1);
  CHECK(alone_sets[0]["pieces"] == 1);
  CHECK(alone_sets[0]["stats"].empty());

  const auto [pair, pair_sets] = attack(R"({"weapon": "Plain Sword", "ability": "War Helm", "attack": 50})");
  CHECK(pair == 55);
  CHECK(pair_sets[0]["pieces"] == 2);

  const auto [full, full_sets] =
      attack(R"({"weapon": "Plain Sword", "ability": "War Helm", "attack": 50, "wearing": [28689, 28690]})");
  CHECK(full == 70);
  CHECK(full_sets[0]["name"] == "Swordsman Set");
  CHECK(full_sets[0]["pieces"] == 4);

  // A lone piece of a set gives nothing, whatever the weapon.
  const auto [other, other_sets] = attack(R"({"weapon": "Burst Bow", "attack": 50, "wearing": [28689]})");
  CHECK(other == 50);
  REQUIRE(other_sets.size() == 1);
  CHECK(other_sets[0]["pieces"] == 1);
}

TEST_CASE("applies a weapon's enchantments") {
  const rotmg::Api api = LoadedApi();
  const auto evaluate = [&](const std::string& enchantments) {
    return json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "attack": 25, "dexterity": 0, "enchantments": )" +
                                    enchantments + "}"));
  };
  const json plain = evaluate("[]");
  REQUIRE(plain["ok"] == true);

  // Damage Bonus IV multiplies each 50-79 roll by 1.05 along with ATT's
  // 1.0, rounded down as weapon damage is.
  const json damage = evaluate(R"(["Damage_Bonus_4"])");
  REQUIRE(damage["ok"] == true);
  double sum = 0;
  for (int roll = 50; roll <= 79; ++roll) {
    sum += std::floor(roll * 1.05);
  }
  CHECK(damage["breakdown"]["subattacks"][0]["damagePerHit"].get<double>() == Catch::Approx(sum / 30));

  // FireRate Bonus IV: 5% more volleys.
  const json rate = evaluate(R"(["FireRate_Bonus_4"])");
  CHECK(rate["breakdown"]["subattacks"][0]["volleysPerSecond"].get<double>() ==
        Catch::Approx(plain["breakdown"]["subattacks"][0]["volleysPerSecond"].get<double>() * 1.05));

  // +1.4 and +0.7 ATT add to 2.1, which rounds down to +2; +1.8 DEX alone
  // rounds down to +1.
  const json stats = evaluate(R"(["Attack_Bonus_1", "Vitality_Bonus_1"])");
  REQUIRE(stats["ok"] == true);
  CHECK(stats["enchantmentStats"] == json::parse(R"([{"stat": "ATT", "amount": 2}])"));
  CHECK(evaluate(R"(["Dexterity_Bonus_2"])")["enchantmentStats"] ==
        json::parse(R"([{"stat": "DEX", "amount": 1}])"));

  // Effects that aren't modelled are named.
  const json odd = evaluate(R"(["Odd_Enchant"])");
  REQUIRE(odd["ok"] == true);
  CHECK(odd["breakdown"]["unsupported"].back() == "Odd Enchant (enchantment)");
}

TEST_CASE("refuses enchantments that can't be on the weapon together") {
  const rotmg::Api api = LoadedApi();
  const auto error = [&](const char* enchantments) {
    const json res = json::parse(api.Evaluate(std::string(R"({"weapon": "Plain Sword", "enchantments": )") +
                                              enchantments + "}"));
    REQUIRE(res["ok"] == false);
    return res["error"].get<std::string>();
  };
  CHECK(error(R"(["Staff_Only"])").find("doesn't go on Plain Sword") != std::string::npos);
  CHECK(error(R"(["Attack_Bonus_1", "Dexterity_Bonus_2"])").find("can't share an item") != std::string::npos);
  CHECK(error(R"(["Nope"])").find("no enchantment named") != std::string::npos);
}

TEST_CASE("an on-shoot boost with a chance is up part of the time") {
  // 3% a shot for +8 ATT over 6 s, 15 s cooldown: once the cooldown ends
  // the next boost comes 1 / (0.03 x shots a second) later on average.
  const rotmg::Api api = LoadedApi();
  const json res = json::parse(
      api.Evaluate(R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 0, "enchantments": ["OnShoot_Attack_Bonus_4"]})"));
  REQUIRE(res["ok"] == true);
  const double shots = res["breakdown"]["subattacks"][0]["volleysPerSecond"].get<double>();
  const double share = 6 / (15 + 1 / (0.03 * shots));
  REQUIRE(res["build"]["uptimes"].size() == 1);
  CHECK(res["build"]["uptimes"][0]["effect"] == "ATT");
  CHECK(res["build"]["uptimes"][0]["share"].get<double>() == Catch::Approx(share));
  CHECK(res["breakdown"]["unsupported"].empty());

  const auto dps = [&](int attack) {
    return json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "dexterity": 0, "attack": )" +
                                    std::to_string(attack) + "}"))["breakdown"]["dps"]
        .get<double>();
  };
  CHECK(res["build"]["weaponDps"].get<double>() == Catch::Approx((1 - share) * dps(50) + share * dps(58)));
}

TEST_CASE("relative enchantments add a share of the bonus stats, all at once") {
  // Swordsman maxes at 75 ATT and 50 DEX. At 95 ATT and 60 DEX with the
  // sword's +4 DEX, bonus ATT is 20 and bonus DEX 14: 16% of 20 plus 13% of
  // 14 is 5.02 ATT, rounded down to 5. The ATT gain doesn't feed back in.
  const rotmg::Api api = LoadedApi();
  const std::string request =
      R"({"weapon": "Plain Sword", "attack": 95, "dexterity": 60,
          "enchantments": ["Relative_Attack_Bonus_4", "Dexterity_To_Attack_Bonus_3"])";
  const json res = json::parse(api.Evaluate(request + R"(, "class": "Swordsman"})"));
  REQUIRE(res["ok"] == true);
  CHECK(res["enchantmentStats"] == json::parse(R"([{"stat": "ATT", "amount": 5}])"));
  CHECK(res["breakdown"]["unsupported"].empty());

  // Without the class there's no max to measure bonus stats from.
  const json classless = json::parse(api.Evaluate(request + "}"));
  REQUIRE(classless["ok"] == true);
  CHECK(classless["enchantmentStats"].empty());
  CHECK(classless["breakdown"]["unsupported"].back() == "relative stat bonuses, which need the class");
}

TEST_CASE("an awakened enchantment swaps in and adds subattacks") {
  // The container's first subattack replaces the sword's attack; its second
  // is added after it. Both bring their own projectiles.
  const rotmg::Api api = LoadedApi();
  const json plain = json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "attack": 25, "dexterity": 0})"));
  const json res = json::parse(
      api.Evaluate(R"({"weapon": "Plain Sword", "attack": 25, "dexterity": 0, "enchantments": ["Awakened_Test"]})"));
  REQUIRE(res["ok"] == true);
  const json& subs = res["breakdown"]["subattacks"];
  REQUIRE(subs.size() == 2);
  CHECK(subs[0]["projectilesPerVolley"] == 3);
  const double volleys = plain["breakdown"]["subattacks"][0]["volleysPerSecond"].get<double>();
  CHECK(subs[0]["volleysPerSecond"].get<double>() == Catch::Approx(volleys * 1.7));
  CHECK(subs[0]["damagePerHit"].get<double>() == Catch::Approx(37));  // rolls 30-44 at 1x
  CHECK(subs[1]["volleysPerSecond"].get<double>() == Catch::Approx(volleys * 0.5));
  CHECK(subs[1]["damagePerHit"].get<double>() == Catch::Approx(10));
  // The container's CollisionMult isn't modelled, and says so.
  const json& unsupported = res["breakdown"]["unsupported"];
  CHECK(std::ranges::find(unsupported, "CollisionMult") != unsupported.end());
}

TEST_CASE("enchantments on the other gear add stats and cut MP costs") {
  const rotmg::Api api = LoadedApi();
  const auto evaluate = [&](const std::string& gear) {
    return json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "ability": "Plain Spell", "wisdom": 50,
                                        "gearEnchantments": )" + gear + "}"));
  };
  // +1.4 ATT on the ring and +0.7 on the armor add to 2.1: +2.
  const json stats = evaluate(R"({"ring": ["Attack_Bonus_1"], "armor": ["Vitality_Bonus_1"]})");
  REQUIRE(stats["ok"] == true);
  CHECK(stats["enchantmentStats"] == json::parse(R"([{"stat": "ATT", "amount": 2}])"));

  const json plain = evaluate("{}");
  const json cheaper = evaluate(R"({"ability": ["MP_Cost_Reduction_4"]})");
  REQUIRE(cheaper["ok"] == true);
  CHECK(cheaper["ability"]["mpCost"].get<double>() == Catch::Approx(plain["ability"]["mpCost"].get<double>() * 0.9));

  const json lucky = evaluate(R"({"ring": ["Lucky_Ring"]})");
  REQUIRE(lucky["ok"] == true);
  CHECK(lucky["breakdown"]["unsupported"].back() == "Lucky Ring (ring enchantment)");

  const json wrong = evaluate(R"({"ring": ["Damage_Bonus_4"]})");
  REQUIRE(wrong["ok"] == false);
  CHECK(wrong["error"].get<std::string>().find("doesn't go on your ring") != std::string::npos);
}

TEST_CASE("same-stat on-shoot boosts on two items count once") {
  // The enchantment "does not stack with other Enchantment procs of the
  // same stat": on the weapon and the ring it's one +8 ATT, up whenever
  // either is.
  const rotmg::Api api = LoadedApi();
  const std::string weapon = R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 0, "enchantments": ["OnShoot_Attack_Bonus_4"])";
  const json one = json::parse(api.Evaluate(weapon + "}"));
  const json two = json::parse(api.Evaluate(weapon + R"(, "gearEnchantments": {"ring": ["OnShoot_Attack_Bonus_4"]}})"));
  REQUIRE(two["ok"] == true);
  REQUIRE(two["build"]["uptimes"].size() == 1);
  const double share = one["build"]["uptimes"][0]["share"].get<double>();
  CHECK(two["build"]["uptimes"][0]["amount"] == 8);
  CHECK(two["build"]["uptimes"][0]["share"].get<double>() == Catch::Approx(1 - (1 - share) * (1 - share)));
}


TEST_CASE("set bonuses count as bonus stats for relative enchantments") {
  // Swordsman maxes at 50 DEX. At 50 DEX the sword's +4 and the full set's
  // +4 make 8 bonus DEX, half of which Relative Dexterity Bonus IV adds.
  const rotmg::Api api = LoadedApi();
  const json res = json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "ability": "War Helm", "class": "Swordsman",
      "attack": 50, "dexterity": 50, "wearing": [28689, 28690],
      "gearEnchantments": {"ring": ["Relative_Dexterity_Bonus_4"]}})"));
  REQUIRE(res["ok"] == true);
  CHECK(res["enchantmentStats"] == json::parse(R"([{"stat": "DEX", "amount": 4}])"));
}


TEST_CASE("enchantments give the same result in any order") {
  const rotmg::Api api = LoadedApi();
  const auto evaluate = [&](const char* enchantments) {
    return json::parse(api.Evaluate(std::string(R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 50,
        "enchantments": )") + enchantments + "}"));
  };
  const json a = evaluate(R"(["FireRate_Bonus_4", "Awakened_Test"])");
  const json b = evaluate(R"(["Awakened_Test", "FireRate_Bonus_4"])");
  REQUIRE(a["ok"] == true);
  CHECK(a["breakdown"] == b["breakdown"]);
  // The fire rate bonus reaches the subattack the awakened enchantment added.
  const json awakened = evaluate(R"(["Awakened_Test"])");
  CHECK(a["breakdown"]["subattacks"][1]["volleysPerSecond"].get<double>() ==
        Catch::Approx(awakened["breakdown"]["subattacks"][1]["volleysPerSecond"].get<double>() * 1.05));
}


TEST_CASE("a class that isn't a string is the same as none") {
  const rotmg::Api api = LoadedApi();
  const json res = json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "class": null,
      "enchantments": ["Relative_Attack_Bonus_4"]})"));
  REQUIRE(res["ok"] == true);
  CHECK(res["breakdown"]["unsupported"].back() == "relative stat bonuses, which need the class");
}

