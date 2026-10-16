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
  CHECK(json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "wearing": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                     14, 15, 16, 17]})"))["error"] == "bad request: more than 16 items worn");
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
  // same stat": a second copy, on the ring, adds nothing (Hugo, 2026-10-12).
  const rotmg::Api api = LoadedApi();
  const std::string weapon = R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 0, "enchantments": ["OnShoot_Attack_Bonus_4"])";
  const json one = json::parse(api.Evaluate(weapon + "}"));
  const json two = json::parse(api.Evaluate(weapon + R"(, "gearEnchantments": {"ring": ["OnShoot_Attack_Bonus_4"]}})"));
  REQUIRE(two["ok"] == true);
  REQUIRE(two["build"]["uptimes"].size() == 1);
  const double share = one["build"]["uptimes"][0]["share"].get<double>();
  CHECK(two["build"]["uptimes"][0]["amount"] == 8);
  CHECK(two["build"]["uptimes"][0]["share"].get<double>() == Catch::Approx(share));
  CHECK(two["build"]["dps"].get<double>() == Catch::Approx(one["build"]["dps"].get<double>()));
  // Of two different ones, the bigger boost is the one that counts: the
  // Alien one's +10, with an Alien item worn, over the +8.
  const json mixed = json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 0,
      "wearing": [28689], "enchantments": ["OnShoot_Attack_Bonus_4"],
      "gearEnchantments": {"ring": ["Alien_OnShoot_Attack_Boost"]}})"));
  REQUIRE(mixed["ok"] == true);
  REQUIRE(mixed["build"]["uptimes"].size() == 1);
  CHECK(mixed["build"]["uptimes"][0]["amount"] == 10);
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


TEST_CASE("a simulation can have several targets") {
  rotmg::Api api = LoadedApi();
  REQUIRE(api.StartSimulation(R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 50,
      "scenario": {"player": {"x": 1, "y": 0}, "aim": 1,
                   "targets": [{"x": 1, "y": 1.5}, {"x": 1, "y": 3, "defense": 40}]}})")
              .empty());
  const json state = json::parse(api.AdvanceSimulation(5000));
  REQUIRE(state["ok"] == true);
  CHECK(state["player"] == json::parse(R"({"x": 1.0, "y": 0.0})"));
  CHECK(state["aim"] == 1);
  REQUIRE(state["targets"].size() == 2);
  CHECK(state["targets"][1]["defense"] == 40);
  // The sword's shots stop at the near dummy before reaching the one aimed at.
  CHECK(state["targets"][0]["hits"].get<int>() > 0);
  CHECK(state["targets"][1]["hits"] == 0);
  CHECK_FALSE(api.StartSimulation(R"({"weapon": "Plain Sword", "scenario": {"targets": []}})").empty());
}

TEST_CASE("simulation targets and the player can move") {
  rotmg::Api api = LoadedApi();
  REQUIRE(api.StartSimulation(R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 50, "speed": 75,
      "scenario": {"player": {"movement": {"kind": "keepDistance", "range": 2}}, "seed": 3,
                   "targets": [{"x": 0, "y": 4, "movement": {"kind": "strafe", "speed": 2, "range": 1}}]}})")
              .empty());
  const json state = json::parse(api.AdvanceSimulation(500));
  REQUIRE(state["ok"] == true);
  CHECK(state["playerSpeed"].get<double>() == Catch::Approx(9.6));
  // Half a second at 2 tiles/s: a tile right of where it started.
  CHECK(state["targets"][0]["x"].get<double>() == Catch::Approx(1).margin(0.01));
  CHECK(state["player"]["y"].get<double>() > 1);
  CHECK_FALSE(api.StartSimulation(R"({"weapon": "Plain Sword",
      "scenario": {"targets": [{"x": 0, "y": 4, "movement": {"kind": "teleport"}}]}})")
                  .empty());
}

TEST_CASE("a simulation casts the ability and reports what it leaves behind") {
  rotmg::Api api = LoadedApi();
  REQUIRE(api.StartSimulation(R"({"weapon": "Plain Sword", "ability": "Toxic Vial", "wisdom": 60,
      "scenario": {"targets": [{"x": 0, "y": 4}]}})")
              .empty());
  json state = json::parse(api.AdvanceSimulation(1000));
  REQUIRE(state["ok"] == true);
  CHECK(state["ability"]["casts"] == 1);
  CHECK(state["mpMax"] == 100);
  CHECK(state["mp"].get<double>() == Catch::Approx(7.7).margin(0.1));
  REQUIRE(state["effects"].size() == 1);
  CHECK(state["effects"][0]["kind"] == "poison");
  CHECK(state["targets"][0]["abilityDamage"].get<double>() > 380);
  CHECK(state["procs"]["activations"] == 0);

  REQUIRE(api.StartSimulation(R"({"weapon": "Plain Sword", "ability": "Toxic Vial", "wisdom": 60,
      "scenario": {"fullMp": false, "targets": [{"x": 0, "y": 4}]}})")
              .empty());
  state = json::parse(api.AdvanceSimulation(1000));
  CHECK(state["ability"]["casts"] == 0);
  CHECK(state["effects"].empty());
}

TEST_CASE("a simulation state carries only the history since the last one") {
  rotmg::Api api = LoadedApi();
  REQUIRE(api.StartSimulation(R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 50,
      "scenario": {"respawn": true, "randomRolls": true, "targets": [{"x": 0, "y": 2, "hp": 500}]}})")
              .empty());
  json state = json::parse(api.AdvanceSimulation(1000));
  CHECK(state["history"]["from"] == 0);
  CHECK(state["history"]["damage"].size() == 10);
  CHECK(state["targets"][0]["kills"].get<int>() >= 1);
  CHECK(state["targets"][0]["killedMs"].is_number());
  CHECK_FALSE(state["hits"].empty());
  state = json::parse(api.AdvanceSimulation(500));
  CHECK(state["history"]["from"] == 10);
  CHECK(state["history"]["mp"].size() == 5);
}

TEST_CASE("a second build can run in the same scene to compare") {
  rotmg::Api api = LoadedApi();
  REQUIRE(api.StartSimulation(R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 50,
      "scenario": {"targets": [{"x": 0, "y": 2}, {"x": 1, "y": 3}]},
      "compare": {"weapon": "Plain Sword", "attack": 75, "dexterity": 50}})")
              .empty());
  const json state = json::parse(api.AdvanceSimulation(3000));
  REQUIRE(state.contains("compare"));
  const json& other = state["compare"];
  CHECK(other["targets"].size() == 2);
  CHECK(other["totals"]["volleys"] == state["totals"]["volleys"]);
  // 75 ATT: (75 + 25) / 50 = 2 against 1.5 at 50.
  CHECK(other["totals"]["damage"].get<double>() ==
        Catch::Approx(state["totals"]["damage"].get<double>() * 2 / 1.5).epsilon(0.02));
  CHECK_FALSE(other.contains("shots"));

  CHECK_FALSE(api.StartSimulation(R"({"weapon": "Plain Sword", "compare": {"weapon": "Nope"}})").empty());
}

TEST_CASE("many runs with random rolls show how time to kill spreads") {
  const rotmg::Api api = LoadedApi();
  const json res = json::parse(api.SimulateMany(R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 50,
      "runs": 30, "seconds": 20, "scenario": {"targets": [{"x": 0, "y": 2, "hp": 3000}]},
      "compare": {"weapon": "Plain Sword", "attack": 75, "dexterity": 50}})"));
  REQUIRE(res["ok"] == true);
  const auto kill = res["timeToKill"].get<std::vector<double>>();
  REQUIRE(kill.size() == 30);
  CHECK(res["unfinished"] == 0);
  CHECK(std::ranges::is_sorted(kill));
  // Rolls differ between seeds, so not every run takes the same time.
  CHECK(kill.front() < kill.back());
  // About 3000 HP at 564 DPS.
  CHECK(kill[15] == Catch::Approx(3000 / 564.0).margin(0.5));
  CHECK(res["compare"]["timeToKill"][15].get<double>() < kill[15]);
}

TEST_CASE("an armor and a ring add their stats and count for sets") {
  rotmg::Api api = LoadedApi();
  const json plain = json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 50})"));
  // Plain Mail +4 ATT, Plain Ring +6 ATT, and with the sword that's 3 of the
  // Swordsman Set: +5 and +5 more ATT. 70 ATT in all.
  const json worn = json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 50,
      "armor": "Plain Mail", "ring": "Plain Ring"})"));
  const json typed = json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "attack": 70, "dexterity": 50})"));
  REQUIRE(worn["ok"] == true);
  CHECK(worn["build"]["dps"].get<double>() == Catch::Approx(typed["build"]["dps"].get<double>()));
  CHECK(worn["build"]["dps"].get<double>() > plain["build"]["dps"].get<double>());
  REQUIRE(worn["sets"].size() == 1);
  CHECK(worn["sets"][0]["pieces"] == 3);
  CHECK(worn["breakdown"]["unsupported"] == json::array({"OnPlayerHitActivate"}));
}

TEST_CASE("enchantments on a named ring follow the ring's own labels") {
  rotmg::Api api = LoadedApi();
  CHECK(json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "ring": "Plain Ring",
      "gearEnchantments": {"ring": ["Lucky_Ring", "Attack_Bonus_1"]}})"))["ok"] == true);
  // The amulet has neither EQUIPMENT nor RING, so it takes neither.
  const json amulet = json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "ring": "Plain Amulet",
      "gearEnchantments": {"ring": ["Lucky_Ring"]}})"));
  CHECK(amulet["ok"] == false);
  CHECK(amulet["error"].get<std::string>().find("Plain Amulet") != std::string::npos);
  CHECK(json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "armor": "Nope"})"))["error"] ==
        "bad request: no armor named \"Nope\"");
}

TEST_CASE("an item takes enchantments from its pool, or by artifact, up to its slots") {
  rotmg::Api api = LoadedApi();
  const auto evaluate = [&](const std::string& extra) {
    return json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "armor": "Plain Mail")" + extra + "}"));
  };
  // Alien enchantments aren't in the pool; an artifact puts them on.
  const json alien = evaluate(R"(, "enchantments": ["Alien_OnShoot_Attack_Boost"])");
  REQUIRE(alien["ok"] == true);
  const json& notes = alien["build"]["assumptions"];
  CHECK(std::ranges::find(notes, json("Alien OnShoot Attack Boost only goes on Plain Sword with an artifact or an "
                                      "engraving")) != notes.end());
  CHECK(evaluate(R"(, "artifacts": false, "enchantments": ["Alien_OnShoot_Attack_Boost"])")["error"] ==
        "bad request: Alien OnShoot Attack Boost only goes on Plain Sword with an artifact");
  CHECK(evaluate(R"(, "enchantments": ["Buzzing_Legacy"])")["error"] ==
        "bad request: Buzzing Bullets (Legacy) is a legacy enchantment, which can't be obtained");
  CHECK(evaluate(R"(, "gearEnchantments": {"armor": ["Attack_Bonus_1", "Vitality_Bonus_1", "Lucky_Ring"]})")["error"] ==
        "bad request: Lucky Ring doesn't go on Plain Mail");
  CHECK(evaluate(R"(, "gearEnchantments": {"armor": ["Attack_Bonus_1", "Vitality_Bonus_1",
                                                     "Relative_Attack_Bonus_4"]})")["error"] ==
        "bad request: Plain Mail holds at most 2 enchantments");
  CHECK(json::parse(api.Evaluate(R"({"weapon": "Venom Katana", "enchantments": ["Attack_Bonus_1"]})"))["error"] ==
        "bad request: Venom Katana can't be enchanted");
}

TEST_CASE("an Alien boost only goes off with Alien gear on") {
  rotmg::Api api = LoadedApi();
  const auto uptimes = [&](const char* armor) {
    return json::parse(api.Evaluate(std::string(R"({"weapon": "Plain Sword", "attack": 50, "dexterity": 50,
        "enchantments": ["Alien_OnShoot_Attack_Boost"], "armor": )") + armor + "}"))["build"]["uptimes"];
  };
  // Its mustWearOR names Plain Mail.
  CHECK(uptimes("null").empty());
  REQUIRE(uptimes(R"("Plain Mail")").size() == 1);
  CHECK(uptimes(R"("Plain Mail")")[0]["amount"] == 10);
}

TEST_CASE("the armor's on-shoot effects go off as the weapon fires") {
  rotmg::Api api = LoadedApi();
  const std::string sword = R"({"weapon": "Plain Sword", "dexterity": 50, "hitModel": {"distance": 3})";
  // The coat also adds 10 ATT, a share of DEX.
  const json plain = json::parse(api.Evaluate(sword + R"(, "attack": 60})"));
  const json coat = json::parse(api.Evaluate(sword + R"(, "attack": 50, "armor": "Spark Coat"})"));
  REQUIRE(coat["ok"] == true);
  // Two 40-49 sparks every 2 seconds, rounded up to whole shots.
  REQUIRE(coat["breakdown"]["procs"].size() == 1);
  const json& sparks = coat["breakdown"]["procs"][0];
  CHECK(sparks["name"] == "Extra shots (Example Coat Spark)");
  CHECK(sparks["damage"].get<double>() == Catch::Approx(2 * 44.5));
  CHECK(coat["build"]["dps"].get<double>() ==
        Catch::Approx(plain["build"]["dps"].get<double>() + sparks["dps"].get<double>()));

  // The simulation fires them too.
  REQUIRE(api.StartSimulation(sword + R"(, "attack": 50, "armor": "Spark Coat"})").empty());
  const json state = json::parse(api.AdvanceSimulation(5000));
  CHECK(state["procs"]["activations"].get<int>() >= 2);
}

TEST_CASE("the armor's effects go off when the ability is used") {
  rotmg::Api api = LoadedApi();
  const std::string build = R"({"weapon": "Plain Sword", "ability": "Plain Spell", "attack": 50, "dexterity": 50,
      "wisdom": 60, "armor": "Spark Coat"})";
  const json res = json::parse(api.Evaluate(build));
  REQUIRE(res["ok"] == true);
  // 7.7 MP a second for the spell's 80 MP, cut to 64 by the coat: a cast
  // every 8.31 seconds.
  const double casts = 7.7 / 64;
  // +15 ATT for 5 seconds on every cast.
  const json& uptimes = res["build"]["uptimes"];
  const auto att = std::ranges::find_if(uptimes, [](const json& u) { return u["effect"] == "ATT"; });
  REQUIRE(att != uptimes.end());
  CHECK((*att)["amount"] == 15);
  CHECK((*att)["share"].get<double>() == Catch::Approx(5 * casts));
  // Three sparks with a 20 second cooldown: every third cast.
  const json& parts = res["ability"]["parts"];
  REQUIRE(parts.size() == 2);
  CHECK(parts[1]["name"] == "On use, Shots at the cursor, 3");
  CHECK(parts[1]["damage"].get<double>() == Catch::Approx(3 * 44.5 / 3));
  // Berserk below half HP doesn't go off at full HP.
  CHECK(std::ranges::none_of(uptimes, [](const json& u) { return u["effect"] == "Berserk"; }));

  // The simulation casts them too.
  REQUIRE(api.StartSimulation(build).empty());
  const json state = json::parse(api.AdvanceSimulation(30000));
  CHECK(state["uptimeMs"].value("ATT", 0.0) > 0);
  CHECK_FALSE(state["uptimeMs"].contains("Berserk"));
}

TEST_CASE("an armor adds a share of one stat to another and cuts MP costs") {
  rotmg::Api api = LoadedApi();
  const std::string build = R"({"weapon": "Plain Sword", "ability": "Plain Spell", "attack": 50, "dexterity": 50,
      "wisdom": 60)";
  const json plain = json::parse(api.Evaluate(build + "}"));
  const json coat = json::parse(api.Evaluate(build + R"(, "armor": "Spark Coat"})"));
  REQUIRE(coat["ok"] == true);
  // 20% of the 54 DEX with the sword's +4 is 10.8 ATT, rounded down.
  CHECK(coat["itemStats"] == json::parse(R"([{"stat": "ATT", "amount": 10}])"));
  CHECK(coat["breakdown"]["terms"][0]["value"] == 60);
  CHECK(coat["ability"]["mpCost"].get<double>() == Catch::Approx(plain["ability"]["mpCost"].get<double>() * 0.8));
  CHECK(coat["breakdown"]["unsupported"].back() == "ActivateOnEquip LootBonus");
}

TEST_CASE("choices give each option's DPS in the slot, null for one the engine rejects") {
  rotmg::Api api = LoadedApi();
  const std::string build = R"({"class": "Swordsman", "weapon": "Plain Sword", "ability": "Plain Spell",
      "ring": "Plain Amulet", "attack": 75, "dexterity": 50, "wisdom": 60)";
  const json res = json::parse(api.EvaluateChoices(build + R"(, "choices": {"slot": "ring", "options": [
      {"item": "Plain Ring"}, {"item": ""}, {"item": "Plain Ring", "enchantments": ["Nonexistent"]}]}})"));
  REQUIRE(res["ok"] == true);
  const auto dps = [&](const std::string& extra) {
    return json::parse(api.Evaluate(build + extra + "}"))["build"]["dps"].get<double>();
  };
  CHECK(res["dps"].get<double>() == Catch::Approx(dps("")));
  REQUIRE(res["options"].size() == 3);
  CHECK(res["options"][0].get<double>() == Catch::Approx(dps(R"(, "ring": "Plain Ring")")));
  CHECK(res["options"][1].get<double>() == Catch::Approx(dps(R"(, "ring": null)")));
  CHECK(res["options"][2].is_null());
  CHECK(json::parse(api.EvaluateChoices(build + R"(, "choices": {"slot": "hat"}})"))["ok"] == false);
}

TEST_CASE("the optimizer finds the build with the most DPS") {
  rotmg::Api api = LoadedApi();
  const std::string start = R"({"class": "Swordsman", "weapon": "Plain Sword", "ability": "Plain Spell",
      "armor": "Plain Mail", "ring": "Plain Amulet", "attack": 75, "dexterity": 50, "wisdom": 60,
      "hitModel": {"distance": 3}, "optimize": {"enchantments": "none"}})";
  const json res = json::parse(api.Optimize(start));
  REQUIRE(res["ok"] == true);
  const json& best = res["builds"][0];
  CHECK(best["dps"].get<double>() >= res["baseline"].get<double>());
  for (std::size_t i = 1; i < res["builds"].size(); ++i) {
    CHECK(res["builds"][i]["dps"].get<double>() <= res["builds"][i - 1]["dps"].get<double>());
  }

  // Every build there is for the Swordsman: one weapon, four abilities, one
  // armor, two rings. The best of them by brute force is the optimizer's.
  double most = 0;
  for (const char* ability : {"Plain Spell", "War Helm", "Battle Seal", "Curse Orb"}) {
    for (const char* ring : {"Plain Ring", "Plain Amulet"}) {
      const json r = json::parse(api.Evaluate(json{{"class", "Swordsman"}, {"weapon", "Plain Sword"},
                                                   {"ability", ability}, {"armor", "Plain Mail"}, {"ring", ring},
                                                   {"attack", 75}, {"dexterity", 50}, {"wisdom", 60},
                                                   {"hitModel", {{"distance", 3}}}}
                                                  .dump()));
      most = std::max(most, r["build"]["dps"].get<double>());
    }
  }
  CHECK(best["dps"].get<double>() == Catch::Approx(most));
  // The ring with ATT, which also makes 4 pieces of the Swordsman Set with
  // the helm, beats the amulet's DEX.
  CHECK(best["ring"] == "Plain Ring");
  // Festive Ring is a reskin of it, and no one can get Relic Helm, so
  // neither is tried.
  for (const json& b : res["builds"]) {
    CHECK(b["ring"] != "Festive Ring");
    CHECK(b["ability"] != "Relic Helm");
  }
}

TEST_CASE("the optimizer ranks by the weapon and ability DPS weighted as asked") {
  rotmg::Api api = LoadedApi();
  const auto optimize = [&](const std::string& weights) {
    return json::parse(api.Optimize(R"({"class": "Swordsman", "weapon": "Plain Sword", "ability": "Plain Spell",
        "armor": "Plain Mail", "ring": "Plain Amulet", "attack": 75, "dexterity": 50, "wisdom": 60,
        "hitModel": {"distance": 3}, "optimize": {"enchantments": "none", "weights": )" +
                                    weights + "}}"));
  };
  const json total = optimize(R"({"weapon": 1, "ability": 1})");
  REQUIRE(total["ok"] == true);
  const json& mixed = total["builds"][0];
  CHECK(mixed["score"].get<double>() == Catch::Approx(mixed["dps"].get<double>()));
  CHECK(mixed["dps"].get<double>() ==
        Catch::Approx(mixed["weaponDps"].get<double>() + mixed["abilityDps"].get<double>()));

  // The most weapon DPS of every Swordsman build, by brute force.
  double most = 0;
  for (const char* ability : {"Plain Spell", "War Helm", "Battle Seal", "Curse Orb"}) {
    for (const char* ring : {"Plain Ring", "Plain Amulet"}) {
      const json r = json::parse(api.Evaluate(json{{"class", "Swordsman"}, {"weapon", "Plain Sword"},
                                                   {"ability", ability}, {"armor", "Plain Mail"}, {"ring", ring},
                                                   {"attack", 75}, {"dexterity", 50}, {"wisdom", 60},
                                                   {"hitModel", {{"distance", 3}}}}
                                                  .dump()));
      most = std::max(most, r["build"]["weaponDps"].get<double>());
    }
  }
  const json weapon = optimize(R"({"weapon": 1, "ability": 0})");
  REQUIRE(weapon["ok"] == true);
  CHECK(weapon["builds"][0]["score"].get<double>() == Catch::Approx(most));
  CHECK(weapon["builds"][0]["weaponDps"].get<double>() == Catch::Approx(most));

  const json none = optimize(R"({"weapon": 0, "ability": 0})");
  CHECK(none["ok"] == false);
}

TEST_CASE("a burst window adds the damage dealt in each") {
  rotmg::Api api = LoadedApi();
  const json res = json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "ability": "Plain Spell",
      "attack": 50, "dexterity": 50, "wisdom": 60, "maxMagicPoints": 200, "burst": {"window": 0, "every": 20}})"));
  REQUIRE(res["ok"] == true);
  const json& burst = res["burst"];
  CHECK(burst["window"] == 0);
  CHECK(burst["every"] == 20);
  CHECK(burst["casts"] == 1);
  CHECK(burst["damage"].get<double>() ==
        Catch::Approx(burst["weaponDamage"].get<double>() + burst["abilityDamage"].get<double>()));
  CHECK(burst["abilityDamage"].get<double>() == Catch::Approx(res["ability"]["damagePerCast"].get<double>()));

  for (const char* bad : {R"({"window": 5, "every": 5})", R"({"window": -1, "every": 5})"}) {
    const json r = json::parse(api.Evaluate(R"({"weapon": "Plain Sword", "burst": )" + std::string(bad) + "}"));
    CHECK(r["ok"] == false);
  }
}

TEST_CASE("the optimizer can rank builds by their damage in a burst window") {
  rotmg::Api api = LoadedApi();
  const std::string stats = R"("attack": 75, "dexterity": 50, "wisdom": 60, "maxMagicPoints": 200,
      "hitModel": {"distance": 3})";
  const json res = json::parse(api.Optimize(R"({"class": "Swordsman", "weapon": "Plain Sword",
      "ability": "Plain Spell", "armor": "Plain Mail", "ring": "Plain Amulet", )" + stats +
                                            R"(, "optimize": {"enchantments": "none",
      "burst": {"window": 1, "every": 10}}})"));
  REQUIRE(res["ok"] == true);
  const json& best = res["builds"][0];
  CHECK(best["score"].get<double>() == Catch::Approx(best["burst"]["damage"].get<double>()));

  // The most damage in a window of every Swordsman build, by brute force.
  double most = 0;
  for (const char* ability : {"Plain Spell", "War Helm", "Battle Seal", "Curse Orb"}) {
    for (const char* ring : {"Plain Ring", "Plain Amulet"}) {
      const json r = json::parse(api.Evaluate(R"({"class": "Swordsman", "weapon": "Plain Sword", "armor": "Plain Mail",
          "ability": ")" + std::string(ability) + R"(", "ring": ")" + ring + R"(", )" + stats +
                                              R"(, "burst": {"window": 1, "every": 10}})"));
      most = std::max(most, r["burst"]["damage"].get<double>());
    }
  }
  CHECK(best["burst"]["damage"].get<double>() == Catch::Approx(most));
}

TEST_CASE("the optimizer's estimate is exact for a search that ends while it's timed") {
  rotmg::Api api = LoadedApi();
  const std::string request = R"({"class": "Swordsman", "weapon": "Plain Sword", "attack": 75, "dexterity": 50,
      "optimize": {"enchantments": "none"}})";
  const json estimate = json::parse(api.EstimateOptimize(request));
  REQUIRE(estimate["ok"] == true);
  const json found = json::parse(api.Optimize(request));
  CHECK(estimate["evaluations"][0] == found["evaluated"]);
  CHECK(estimate["evaluations"][1] == found["evaluated"]);
  CHECK(estimate["seconds"][0].get<double>() > 0);
  CHECK(estimate["seconds"][0] == estimate["seconds"][1]);
  CHECK(json::parse(api.EstimateOptimize(R"({"weapon": "Plain Sword"})"))["ok"] == false);
}

TEST_CASE("explaining a build gives what each piece and stat point adds") {
  rotmg::Api api = LoadedApi();
  const std::string build = R"({"class": "Swordsman", "weapon": "Plain Sword", "ability": "Battle Seal",
      "ring": "Plain Ring", "attack": 75, "dexterity": 50, "wisdom": 60, "hitModel": {"distance": 3})";
  const json res = json::parse(api.ExplainBuild(build + "}"));
  REQUIRE(res["ok"] == true);
  const auto dps = [&](const std::string& extra) {
    return json::parse(api.Evaluate(build + extra + "}"))["build"]["dps"].get<double>();
  };
  // The score is total DPS with a millionth of it again to break ties.
  CHECK(res["score"].get<double>() == Catch::Approx(dps("") * (1 + 1e-6)));
  CHECK(res["items"]["ring"].get<double>() ==
        Catch::Approx((dps("") - dps(R"(, "ring": null)")) * (1 + 1e-6)));
  CHECK_FALSE(res["items"].contains("weapon"));
  CHECK_FALSE(res["items"].contains("armor"));
  // ATT raises the sword's damage; the rest of the stats do nothing here.
  const json& stats = res["stats"];
  REQUIRE(stats.size() == 8);
  CHECK(stats[0]["stat"] == "ATT");
  CHECK(stats[0]["perPoint"].get<double>() > 0);
  CHECK(stats[4]["perPoint"].get<double>() == Catch::Approx(0).margin(1e-9));
}

TEST_CASE("the optimizer works from a reskin or an unobtainable item you chose") {
  rotmg::Api api = LoadedApi();
  const auto optimize = [&](const std::string& extra) {
    const json res = json::parse(api.Optimize(R"({"class": "Swordsman", "weapon": "Plain Sword",
        "ability": "Relic Helm", "armor": "Plain Mail", "ring": "Festive Ring",
        "attack": 75, "dexterity": 50, "wisdom": 60,
        "optimize": {"enchantments": "best")" + extra + "}}"));
    REQUIRE(res["ok"] == true);
    return res;
  };
  // Locked, they stay, and the reskin gets its enchantments.
  const json locked = optimize(R"(, "lock": ["ability", "ring"])");
  const json& best = locked["builds"][0];
  CHECK(best["ability"] == "Relic Helm");
  CHECK(best["ring"] == "Festive Ring");
  CHECK_FALSE(best["enchantments"]["ring"].empty());
  CHECK(best["enchantments"]["ability"].empty());
  // Unlocked, the search starts from them and returns nothing worse.
  const json open = optimize("");
  const json own = json::parse(api.Evaluate(R"({"class": "Swordsman", "weapon": "Plain Sword",
      "ability": "Relic Helm", "armor": "Plain Mail", "ring": "Festive Ring",
      "attack": 75, "dexterity": 50, "wisdom": 60})"));
  CHECK(open["baseline"].get<double>() == Catch::Approx(own["build"]["dps"].get<double>()));
  CHECK(open["builds"][0]["dps"].get<double>() >= open["baseline"].get<double>());
}

TEST_CASE("the optimizer keeps locked slots and fills enchantments legally") {
  rotmg::Api api = LoadedApi();
  const json locked = json::parse(api.Optimize(R"({"class": "Swordsman", "weapon": "Plain Sword",
      "ring": "Plain Amulet", "attack": 75, "dexterity": 50,
      "optimize": {"lock": ["ring", "ability", "armor"], "enchantments": "keep"}})"));
  REQUIRE(locked["ok"] == true);
  CHECK(locked["builds"][0]["ring"] == "Plain Amulet");
  CHECK(locked["builds"][0]["dps"].get<double>() == Catch::Approx(locked["baseline"].get<double>()));

  const json filled = json::parse(api.Optimize(R"({"class": "Swordsman", "weapon": "Plain Sword",
      "attack": 75, "dexterity": 50, "optimize": {"lock": ["ability", "armor", "ring"], "enchantments": "best"}})"));
  REQUIRE(filled["ok"] == true);
  const json& best = filled["builds"][0];
  CHECK(best["dps"].get<double>() > filled["baseline"].get<double>());
  const auto on_weapon = best["enchantments"]["weapon"].get<std::vector<std::string>>();
  CHECK_FALSE(on_weapon.empty());
  // 2 per item unless the request asks for more.
  CHECK(on_weapon.size() <= 2);
  const json four = json::parse(api.Optimize(R"({"class": "Swordsman", "weapon": "Plain Sword", "attack": 75,
      "dexterity": 50, "optimize": {"lock": ["ability", "armor", "ring"], "enchantments": "best", "slots": 4}})"));
  REQUIRE(four["ok"] == true);
  const json& on_four = four["builds"][0]["enchantments"]["weapon"];
  CHECK(on_four.size() <= 4);
  CHECK(std::ranges::find(on_four, json("Damage_Bonus_4")) != on_four.end());
  CHECK(four["builds"][0]["dps"].get<double>() >= best["dps"].get<double>());
  // Two single-stat bonuses can't share an item.
  const auto has = [&](const char* id) { return std::ranges::find(on_weapon, id) != on_weapon.end(); };
  CHECK_FALSE((has("Attack_Bonus_1") && has("Dexterity_Bonus_2")));
  // What it found evaluates to the same DPS on its own.
  json check = {{"class", "Swordsman"}, {"weapon", "Plain Sword"}, {"attack", 75}, {"dexterity", 50},
                {"enchantments", on_weapon}};
  CHECK(json::parse(api.Evaluate(check.dump()))["build"]["dps"].get<double>() ==
        Catch::Approx(best["dps"].get<double>()));
  CHECK(json::parse(api.Optimize(R"({"weapon": "Plain Sword"})"))["ok"] == false);
}

TEST_CASE("the optimizer only uses enchantments the items can get") {
  rotmg::Api api = LoadedApi();
  const auto optimize = [&](const char* artifacts) {
    const json res = json::parse(api.Optimize(std::string(R"({"class": "Swordsman", "weapon": "Plain Sword",
        "armor": "Plain Mail", "attack": 75, "dexterity": 50,
        "optimize": {"lock": ["weapon", "ability", "armor", "ring"], "enchantments": "best", "artifacts": )") +
                                              artifacts + "}}"));
    REQUIRE(res["ok"] == true);
    return res["builds"][0]["enchantments"];
  };
  const auto uses = [](const json& enchantments, const char* id) {
    return std::ranges::any_of(enchantments.items(), [&](const auto& slot) {
      return std::ranges::find(slot.value(), json(id)) != slot.value().end();
    });
  };
  for (const char* artifacts : {"true", "false"}) {
    const json found = optimize(artifacts);
    CHECK_FALSE(uses(found, "Buzzing_Legacy"));
    CHECK(found["armor"].size() <= 2);
  }
  // Its 10 ATT boost beats the 8 of the one that rolls.
  CHECK(uses(optimize("true"), "Alien_OnShoot_Attack_Boost"));
  CHECK_FALSE(uses(optimize("false"), "Alien_OnShoot_Attack_Boost"));
}

TEST_CASE("the exact search proves the best armor, ring and enchantments for a weapon and ability") {
  rotmg::Api api = LoadedApi();
  const auto optimize = [&](const std::string& exact) {
    return json::parse(api.Optimize(R"({"class": "Swordsman", "weapon": "Plain Sword", "ability": "Plain Spell",
        "armor": "Plain Mail", "attack": 75, "dexterity": 50, "wisdom": 60, "hitModel": {"distance": 3},
        "optimize": {"slots": 1, "exact": )" + exact + "}}"));
  };
  const json res = optimize(R"({"rings": ["Plain Ring", "Plain Amulet"]})");
  REQUIRE(res["ok"] == true);
  CHECK(res["proven"] == true);
  CHECK(res["pairs"] == 2);
  CHECK(res["builds"][0]["score"].get<double>() >= res["baseline"].get<double>());
  const json& best = res["builds"][0];
  CHECK(best["weapon"] == "Plain Sword");
  CHECK(best["ability"] == "Plain Spell");
  CHECK(best["armor"] == "Plain Mail");
  // No other choice beats it: each ring on its own does no better.
  for (const char* ring : {"Plain Ring", "Plain Amulet"}) {
    const json one = optimize(std::string(R"({"rings": [")") + ring + R"("]})");
    REQUIRE(one["ok"] == true);
    CHECK(one["builds"][0]["score"].get<double>() <= best["score"].get<double>());
  }

  CHECK(json::parse(api.Optimize(R"({"class": "Swordsman", "weapon": "Plain Sword",
      "optimize": {"exact": {}}})"))["ok"] == false);
  CHECK(optimize(R"({"rings": ["No Such Ring"]})")["ok"] == false);

  // The baseline is the request's build as it is, enchantments and all: the
  // best build, proven again, is no gain on itself.
  const json& e = best["enchantments"];
  const json again = json::parse(api.Optimize(json{{"class", "Swordsman"}, {"weapon", best["weapon"]},
                                                   {"ability", best["ability"]}, {"armor", best["armor"]},
                                                   {"ring", best["ring"]}, {"attack", 75}, {"dexterity", 50},
                                                   {"wisdom", 60}, {"hitModel", {{"distance", 3}}},
                                                   {"enchantments", e["weapon"]},
                                                   {"gearEnchantments", {{"ability", e["ability"]},
                                                                         {"armor", e["armor"]},
                                                                         {"ring", e["ring"]}}},
                                                   {"optimize", {{"slots", 1}, {"exact", json::object()}}}}
                                                  .dump()));
  REQUIRE(again["ok"] == true);
  CHECK(again["builds"][0]["score"].get<double>() == Catch::Approx(again["baseline"].get<double>()));
  CHECK(json::parse(api.Optimize(R"({"class": "Swordsman", "weapon": "Plain Sword", "ability": "Plain Spell",
      "optimize": {"exact": {}, "burst": {"window": 1, "every": 10}}})"))["ok"] == false);
}
