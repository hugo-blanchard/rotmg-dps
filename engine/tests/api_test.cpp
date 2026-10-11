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
