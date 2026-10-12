#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
