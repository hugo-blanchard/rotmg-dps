#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "fixture.hpp"
#include "rotmg/evaluate.hpp"

using Catch::Approx;
using rotmg::EvaluateWeapon;

namespace {

const rotmg::Dataset& Data() {
  static const rotmg::Dataset dataset = LoadSmallDataset();
  return dataset;
}

const rotmg::Weapon& Weapon(const char* name) {
  const rotmg::Weapon* w = rotmg::FindWeapon(Data(), name);
  REQUIRE(w != nullptr);
  return *w;
}

bool HasAssumption(const rotmg::Breakdown& b, const std::string& text) {
  return std::ranges::find(b.assumptions, text) != b.assumptions.end();
}

}  // namespace

TEST_CASE("a single-shot weapon: volleys per second times average hit") {
  // Plain Sword: 50-80, +4 DEX. At 25 ATT the multiplier is 1, so the
  // average hit is (50 + 79) / 2 = 64.5.
  const auto b = EvaluateWeapon(Weapon("Plain Sword"), {.attack = 25, .dexterity = 0}, {});
  const double volleys = 1.5 + 6.5 * 4 / 75.0;
  REQUIRE(b.subattacks.size() == 1);
  CHECK(b.subattacks[0].volleys_per_second == Approx(volleys));
  CHECK(b.subattacks[0].damage_per_hit == Approx(64.5));
  CHECK(b.dps == Approx(volleys * 64.5));
  CHECK(HasAssumption(b, "every projectile hits"));
}

TEST_CASE("subattacks fire independently and add up") {
  // Burst Bow at 75 DEX: 7.2 volleys/s uncapped, burst cap 4 / 0.8 = 5.
  const auto b = EvaluateWeapon(Weapon("Burst Bow"), {.attack = 25, .dexterity = 75}, {});
  REQUIRE(b.subattacks.size() == 2);
  CHECK(b.subattacks[0].volleys_per_second == Approx(5.0));
  CHECK(b.subattacks[0].hits_per_volley == 2);
  CHECK(b.subattacks[0].damage_per_hit == Approx(114.5));
  CHECK(b.subattacks[1].damage_per_hit == Approx(44.5));
  CHECK(b.dps == Approx(5.0 * 2 * 114.5 + 5.0 * 1 * 44.5));
}

TEST_CASE("armor-piercing projectiles ignore DEF") {
  const auto open = EvaluateWeapon(Weapon("Burst Bow"), {.attack = 25, .dexterity = 75}, {});
  const auto armored = EvaluateWeapon(Weapon("Burst Bow"), {.attack = 25, .dexterity = 75}, {.defense = 40});
  CHECK(armored.subattacks[1].dps == Approx(open.subattacks[1].dps));
  CHECK(armored.subattacks[0].dps < open.subattacks[0].dps);
}

TEST_CASE("hits per volley can be set per subattack") {
  rotmg::Assumptions a;
  a.hits_per_volley = {1.0};
  const auto b = EvaluateWeapon(Weapon("Burst Bow"), {.attack = 25, .dexterity = 75}, {}, a);
  CHECK(b.subattacks[0].hits_per_volley == 1.0);
  CHECK(b.subattacks[1].hits_per_volley == 1.0);
  // Subattack 1 still uses the default.
  CHECK(HasAssumption(b, "every projectile hits"));

  a.hits_per_volley = {1.0, 0.0};
  CHECK_FALSE(HasAssumption(EvaluateWeapon(Weapon("Burst Bow"), {}, {}, a), "every projectile hits"));
}

TEST_CASE("unsupported mechanics are passed through") {
  const auto b = EvaluateWeapon(Weapon("Proc Wand"), {.attack = 50, .dexterity = 50}, {});
  CHECK(b.unsupported == std::vector<std::string>{"OnPlayerShootActivate"});
}

TEST_CASE("Berserk on a burst weapon notes the burst delay assumption") {
  const auto b = EvaluateWeapon(Weapon("Burst Bow"), {.attack = 25, .dexterity = 75, .status = {.berserk = true}}, {});
  CHECK(HasAssumption(b, "Berserk multiplies burst delay by 0.67"));
}

TEST_CASE("hits come from the user, then a curated default, then the estimate") {
  rotmg::Assumptions a;
  a.distance = 3;
  a.curated_hits_per_volley = {std::nullopt, 0.5};
  a.hits_per_volley = {1.0};
  const auto both = EvaluateWeapon(Weapon("Burst Bow"), {.attack = 25, .dexterity = 75}, {}, a);
  CHECK(both.subattacks[0].hits_source == rotmg::Source::kUser);
  CHECK(both.subattacks[1].hits_source == rotmg::Source::kCurated);
  CHECK(both.subattacks[1].hits_per_volley == 0.5);

  a.hits_per_volley.clear();
  const auto estimated = EvaluateWeapon(Weapon("Burst Bow"), {.attack = 25, .dexterity = 75}, {}, a);
  CHECK(estimated.subattacks[0].hits_source == rotmg::Source::kGeometricEstimate);
  CHECK(estimated.subattacks[0].hits_per_volley <= 2);
  CHECK(HasAssumption(estimated, "the hit estimate aims perfectly at a target that doesn't move"));
  CHECK_FALSE(HasAssumption(estimated, "every projectile hits"));
}

TEST_CASE("weapons the estimate can't follow fall back to every projectile hitting") {
  rotmg::Weapon w = Weapon("Plain Sword");
  w.projectiles[0].parametric = true;
  rotmg::Assumptions a;
  a.distance = 3;
  const auto b = EvaluateWeapon(w, {.attack = 25, .dexterity = 50}, {}, a);
  CHECK(b.subattacks[0].hits_source == rotmg::Source::kAssumed);
  CHECK(b.subattacks[0].hits_per_volley == 1);
  CHECK(HasAssumption(b, "every projectile hits, since the hit estimate can't follow figure-eight (parametric) shots"));
}

TEST_CASE("a burst delay shared by every subattack is listed once") {
  const auto b = EvaluateWeapon(Weapon("Burst Bow"), {.attack = 25, .dexterity = 75}, {});
  const auto delays = std::ranges::count_if(b.terms, [](const rotmg::Term& t) { return t.name.ends_with("urst delay"); });
  CHECK(delays == 1);
  CHECK(std::ranges::find(b.terms, std::string("Burst delay"), &rotmg::Term::name) != b.terms.end());
}

TEST_CASE("characters add gear bonuses to any stat") {
  const rotmg::Character c{.attack = 50, .dexterity = 40, .wisdom = 30};
  const rotmg::Character geared = c.With({{"WIS", 6}, {"ATT", 2}, {"MAXHP", 20}, {"LUCK", 9}});
  CHECK(geared.Stat("WIS") == 36);
  CHECK(geared.Stat("ATT") == 52);
  CHECK(geared.Stat("DEX") == 40);
  CHECK(geared.Stat("MAXHP") == 20);
  CHECK(geared.Stat("LUCK") == 0);
}
