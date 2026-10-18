#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "fixture.hpp"
#include "rotmg/ability.hpp"
#include "rotmg/evaluate.hpp"
#include "rotmg/sim.hpp"

using Catch::Approx;
using rotmg::Scenario;
using rotmg::Simulation;

namespace {

const rotmg::Dataset& Data() {
  static const rotmg::Dataset dataset = LoadSmallDataset();
  return dataset;
}

const rotmg::Weapon& Sword() {
  const rotmg::Weapon* w = rotmg::FindWeapon(Data(), "Plain Sword");
  REQUIRE(w != nullptr);
  return *w;
}

const rotmg::Ability& Ability(const char* name) {
  const rotmg::Ability* a = rotmg::FindAbility(Data(), name);
  REQUIRE(a != nullptr);
  return *a;
}

const rotmg::Character kWis60{.attack = 50, .dexterity = 50, .wisdom = 60};

// Still targets at these points, the player at the origin aiming at the
// first.
Scenario At(std::vector<rotmg::Point> points, rotmg::Target target = {}) {
  Scenario s;
  for (const rotmg::Point& p : points) {
    s.targets.push_back({p, 0.5, target});
  }
  return s;
}

double AbilityDamage(const Simulation& sim, std::size_t target) { return sim.PerTarget()[target].ability_damage; }

bool HasTrap(const Simulation& sim) {
  for (const auto& e : sim.Effects()) {
    if (e.kind == Simulation::Effect::Kind::kTrap) {
      return true;
    }
  }
  return false;
}

// The scepter's bolt among the effects in the world.
const Simulation::Effect& Bolt(const Simulation& sim) {
  const auto it = std::ranges::find(sim.Effects(), Simulation::Effect::Kind::kBolt, &Simulation::Effect::kind);
  REQUIRE(it != sim.Effects().end());
  return *it;
}

}  // namespace

TEST_CASE("casting as often as MP allows matches the evaluated ability DPS") {
  const rotmg::Ability& spell = Ability("Plain Spell");
  Scenario s = At({{0, 3}});
  s.full_mp = false;
  Simulation sim(Sword(), kWis60, s, &spell);
  sim.Advance(100'000);
  // 7.7 MP a second and 80 a cast: the first cast is 10.4 seconds in.
  const auto b = rotmg::EvaluateAbility(spell, kWis60, {}, {});
  CHECK(sim.AbilityTotals().casts == 9);
  CHECK(sim.AbilityTotals().hits == 9 * 17);
  CHECK(sim.AbilityTotals().damage == Approx(9 * b.damage_per_cast));
  CHECK(sim.Mp() == Approx(100 * 7.7 - 9 * 80).margin(0.1));
}

TEST_CASE("a full MP bar is spent on the first step") {
  Simulation sim(Sword(), kWis60, At({{0, 3}}), &Ability("Plain Spell"));
  sim.Advance(10);
  CHECK(sim.AbilityTotals().casts == 1);
  CHECK(sim.Mp() == Approx(0).margin(0.1));
}

TEST_CASE("Scepters: lightning chains through targets within 8 tiles") {
  // 60 WIS is 10 over 50: 300 + 8 * 10, then 40 less on each next target.
  // They stand more than a shockblast apart. The fourth target is more than
  // 8 tiles from all the others, so the chain stops.
  Simulation sim(Sword(), kWis60, At({{0, 4}, {0, 8}, {1, 13}, {12, 0}}), &Ability("Shock Scepter"));
  sim.Advance(10);
  CHECK(sim.AbilityTotals().hits == 3);
  CHECK(AbilityDamage(sim, 0) == Approx(380));
  CHECK(AbilityDamage(sim, 1) == Approx(340));
  CHECK(AbilityDamage(sim, 2) == Approx(300));
  CHECK(AbilityDamage(sim, 3) == 0);
  CHECK(Bolt(sim).chain.size() == 4);
}

TEST_CASE("Scepters: the chain starts at the closest target in the cone") {
  // Aiming at the far target, the near one in line with it is hit first; the
  // one off to the side is outside the 30 degree cone.
  Scenario s = At({{0, 8}, {0, 3}, {-2, 1}});
  Simulation sim(Sword(), kWis60, s, &Ability("Shock Scepter"));
  sim.Advance(10);
  const auto& chain = Bolt(sim).chain;
  REQUIRE(chain.size() == 4);
  CHECK(chain[1].y == Approx(3));
}

TEST_CASE("Scepters: a struck enemy's shockblast hits the others near it") {
  // Two targets 2 tiles apart, inside each other's 2.5 tile shockblast: each
  // takes three pulses of 75 + 2.2 * 10 from the other over a second, armor
  // piercing. Alone, a target takes only the lightning.
  Simulation pair(Sword(), kWis60, At({{0, 4}, {0, 6}}, {.defense = 50}), &Ability("Shock Scepter"));
  pair.Advance(1100);
  CHECK(AbilityDamage(pair, 0) == Approx(380 - 50 + 3 * 97));
  CHECK(AbilityDamage(pair, 1) == Approx(340 - 50 + 3 * 97));
  Simulation alone(Sword(), kWis60, At({{0, 4}}, {.defense = 50}), &Ability("Shock Scepter"));
  alone.Advance(1100);
  CHECK(AbilityDamage(alone, 0) == Approx(380 - 50));
}

TEST_CASE("Poisons: land after 0.8 seconds, ignore DEF and spread their damage") {
  // 60 WIS: impact 350 + 30, then 1050 + 180 over 4.5 seconds. The second
  // target is outside the poison.
  Simulation sim(Sword(), kWis60, At({{0, 4}, {3, 4}}, {.defense = 100}), &Ability("Toxic Vial"));
  sim.Advance(790);
  CHECK(AbilityDamage(sim, 0) == 0);
  sim.Advance(20);
  CHECK(AbilityDamage(sim, 0) == Approx(380).margin(10));
  sim.Advance(2250);
  CHECK(AbilityDamage(sim, 0) == Approx(380 + 1230 / 2.0).margin(10));
  sim.Advance(5000);
  CHECK(AbilityDamage(sim, 0) == Approx(1610).margin(1));
  CHECK(AbilityDamage(sim, 1) == 0);
}

TEST_CASE("Traps: one thrown onto a target goes off as it lands") {
  // 26 WIS over 34 adds 5 each: 460 + 130, less 40 DEF.
  Simulation sim(Sword(), kWis60, At({{0, 4}}, {.defense = 40}), &Ability("Snare Trap"));
  sim.Advance(990);
  CHECK(AbilityDamage(sim, 0) == 0);
  sim.Advance(20);
  CHECK(AbilityDamage(sim, 0) == Approx(550));
  CHECK(sim.AbilityTotals().casts == 1);
}

TEST_CASE("Traps: one that lands clear waits for a target to come close") {
  // Circling 3 tiles round at 4 tiles a second, the target is 3.7 tiles from
  // the trap when it lands, outside the 2.25 tile trigger, and comes back
  // within it about 4 seconds in.
  Scenario s = At({{0, 4}});
  s.targets[0].movement = {rotmg::Movement::Kind::kCircle, 4, 3, 0, true};
  Simulation sim(Sword(), kWis60, s, &Ability("Snare Trap"));
  sim.Advance(1010);
  CHECK(AbilityDamage(sim, 0) == 0);
  CHECK(HasTrap(sim));
  sim.Advance(2900);
  CHECK(AbilityDamage(sim, 0) == 0);
  sim.Advance(1000);
  // The trap's 590, and its sticky bomb's 845 0.8 seconds later.
  CHECK(AbilityDamage(sim, 0) == Approx(590 + 845));
  CHECK_FALSE(HasTrap(sim));
}

TEST_CASE("Traps: one nothing comes near goes off when it runs out") {
  // The target walks straight away at 4 tiles a second.
  Scenario s = At({{0, 4}});
  s.targets[0].movement = {rotmg::Movement::Kind::kChargeRetreat, 4, 4, 40, true};
  Simulation sim(Sword(), kWis60, s, &Ability("Snare Trap"));
  sim.Advance(10'900);
  CHECK(HasTrap(sim));
  sim.Advance(200);
  CHECK_FALSE(HasTrap(sim));
  CHECK(AbilityDamage(sim, 0) == 0);
}

TEST_CASE("Traps: a sticky bomb goes off on the closest target caught") {
  // Both targets are caught by the trap, 550 each through 40 DEF. The bomb
  // sticks to the one the trap landed on and, 0.8 seconds later, hits both
  // within its 3.375 tiles for 715 + 5 * 26 - 40.
  Simulation sim(Sword(), kWis60, At({{0, 4}, {2, 4}}, {.defense = 40}), &Ability("Snare Trap"));
  sim.Advance(1010);
  CHECK(AbilityDamage(sim, 0) == Approx(550));
  CHECK(AbilityDamage(sim, 1) == Approx(550));
  sim.Advance(800);
  CHECK(AbilityDamage(sim, 0) == Approx(550 + 805));
  CHECK(AbilityDamage(sim, 1) == Approx(550 + 805));
}

TEST_CASE("Tomes: two blasts 0.8 seconds apart") {
  Simulation sim(Sword(), kWis60, At({{0, 4}, {2, 4}, {4, 4}}), &Ability("Holy Tome"));
  sim.Advance(10);
  CHECK(AbilityDamage(sim, 0) == Approx(640));
  CHECK(AbilityDamage(sim, 1) == Approx(640));
  CHECK(AbilityDamage(sim, 2) == 0);
  sim.Advance(800);
  CHECK(AbilityDamage(sim, 0) == Approx(1280));
}

TEST_CASE("a quiver's arrows fly from the player and expose what they hit") {
  // The outer arrows pass 0.46 tiles from a target 2.5 tiles away and miss
  // one 3 tiles away (see the evaluation's test). They land a step after the
  // inner pair, which has exposed the target by then: 20 more damage each.
  for (const auto& [distance, hits, exposed] : {std::tuple{2.5, 4L, 2}, std::tuple{3.0, 2L, 0}}) {
    Simulation sim(Sword(), kWis60, At({{0, distance}}), &Ability("Plain Quiver"));
    sim.Advance(1000);
    INFO(distance << " tiles");
    CHECK(sim.AbilityTotals().hits == hits);
    CHECK(AbilityDamage(sim, 0) == Approx(static_cast<double>(hits) * (314.5 + 3.88 * 26) + exposed * 20));
    REQUIRE(sim.TargetStatuses(0).size() == 1);
    CHECK(sim.TargetStatuses(0)[0] == "Exposed");
  }
}

TEST_CASE("a wakizashi's slashes cross the cursor") {
  // Four slashes and the afterimage; the third activate's item isn't in the
  // dataset. The first slash through exposes the target for the other four.
  Simulation sim(Sword(), kWis60, At({{0, 3}}), &Ability("Plain Wakizashi"));
  sim.Advance(1000);
  CHECK(sim.AbilityTotals().hits == 5);
  CHECK(AbilityDamage(sim, 0) == Approx(4 * (399.5 + 54) + 149.5 + 4 * 20));
}

TEST_CASE("Berserk from a helm speeds up the weapon while it lasts") {
  // 60 WIS gives 7 seconds of Berserk; MP for the next cast takes longer.
  Simulation plain(Sword(), kWis60, At({{0, 3}}));
  Simulation helm(Sword(), kWis60, At({{0, 3}}), &Ability("War Helm"));
  plain.Advance(6000);
  helm.Advance(6000);
  CHECK(helm.PlayerStatuses() == std::vector<std::string>{"Berserk"});
  const auto volleys = [](const Simulation& sim) { return static_cast<double>(sim.Totals().volleys); };
  CHECK(volleys(helm) == Approx(1.25 * volleys(plain)).margin(2));
  helm.Advance(2000);
  CHECK(helm.PlayerStatuses().empty());
}

TEST_CASE("Curse from an orb adds a quarter to the weapon's damage") {
  // 60 WIS curses for 4.5 seconds.
  Simulation plain(Sword(), kWis60, At({{0, 3}}));
  Simulation orb(Sword(), kWis60, At({{0, 3}}), &Ability("Curse Orb"));
  plain.Advance(4000);
  orb.Advance(4000);
  REQUIRE(plain.Totals().damage > 0);
  CHECK(orb.Totals().damage == Approx(1.25 * plain.Totals().damage));
  CHECK(orb.TargetStatuses(0) == std::vector<std::string>{"Curse"});
}

TEST_CASE("the weapon's procs go off in the simulation as often as evaluated") {
  // Poison bursts around the player and extra shots at the cursor, on a
  // target 2 tiles away: both reach it every time.
  const rotmg::Weapon* katana = rotmg::FindWeapon(Data(), "Venom Katana");
  REQUIRE(katana != nullptr);
  const rotmg::Character c{.attack = 50, .dexterity = 50};
  Simulation sim(*katana, c, At({{0, 2}}));
  sim.Advance(300'000);
  const auto b = rotmg::EvaluateWeapon(*katana, c, {});
  double procs = 0;
  for (const rotmg::ProcResult& p : b.procs) {
    procs += p.dps;
  }
  REQUIRE(procs > 0);
  CHECK(sim.ProcTotals().damage / 300 == Approx(procs).epsilon(0.01));
  CHECK(sim.AbilityTotals().damage == 0);

  // The ring's two shots on every shot only with the ring on.
  const rotmg::Character ringed{.attack = 50, .dexterity = 50, .worn = {0x7002}};
  Simulation with_ring(*katana, ringed, At({{0, 2}}));
  with_ring.Advance(10'000);
  CHECK(with_ring.ProcTotals().hits > 2 * with_ring.Totals().volleys);
}

TEST_CASE("a chance boost is up for its share of the time") {
  // 5% a shot for +5 ATT over 4 s, then a 10 s cooldown. At 5.83 shots a
  // second the chance adds up after 20 shots, 3.43 s, so it's up 4 s in
  // every 13.43.
  rotmg::Weapon sword = Sword();
  rotmg::Activate boost;
  boost.type = "StatBoostSelf";
  for (const auto& [key, value] : std::initializer_list<std::pair<const char*, rotmg::Activate::Value>>{
           {"proc", 0.05}, {"cooldown", 10.0}, {"stat", std::string("ATT")}, {"amount", 5.0}, {"duration", 4.0}}) {
    boost.params.emplace(key, value);
  }
  sword.on_shoot.push_back(boost);
  Simulation sim(sword, {.attack = 50, .dexterity = 50}, At({{0, 3}}));
  int up = 0;
  const int samples = 6000;
  for (int i = 0; i < samples; ++i) {
    sim.Advance(100);
    up += sim.PlayerStatuses() == std::vector<std::string>{"+5 ATT"} ? 1 : 0;
  }
  CHECK(static_cast<double>(up) / samples == Approx(4 / (10 + 20 / (1.5 + 6.5 * 50 / 75.0))).margin(0.01));
}

TEST_CASE("the time each buff and debuff is up is measured") {
  // Berserk for 7 s from a full bar; the next cast needs 90 MP at 7.7 a
  // second, 11.7 s later.
  Simulation helm(Sword(), kWis60, At({{0, 3}}), &Ability("War Helm"));
  helm.Advance(10'000);
  CHECK(helm.PlayerUptime().at("Berserk") == Approx(7000).margin(10));

  // Curse for 4.5 s on the target in the orb's radius.
  Simulation orb(Sword(), kWis60, At({{0, 3}}), &Ability("Curse Orb"));
  orb.Advance(10'000);
  CHECK(orb.TargetUptime(0).at("Curse") == Approx(4500).margin(10));
}

TEST_CASE("a poison makes the weapon do more to what it poisons") {
  // From when the vial lands, 0.8 s in, the sword's hits on the poisoned
  // target do 11.75% more for 4.5 s.
  Simulation plain(Sword(), kWis60, At({{0, 3}}));
  Simulation poisoned(Sword(), kWis60, At({{0, 3}}), &Ability("Toxic Vial"));
  plain.Advance(1000);
  poisoned.Advance(1000);
  const double before = poisoned.Totals().damage;
  CHECK(before > plain.Totals().damage);
  const double plain_before = plain.Totals().damage;
  plain.Advance(3000);
  poisoned.Advance(3000);
  CHECK(poisoned.Totals().damage - before == Approx(1.1175 * (plain.Totals().damage - plain_before)));
  CHECK(poisoned.TargetStatuses(0) == std::vector<std::string>{rotmg::kPoisonBoost});
}

TEST_CASE("a rocket on hit explodes on the target and what's around it") {
  // 25% a hit past a 0.4 s cooldown: with the chances adding up, every
  // fourth hit after the cooldown, about once a second at 50 DEX with the
  // sword's. The
  // second target is in the blast but off the sword's line; the third is
  // past both.
  rotmg::Weapon rifle = Sword();
  rifle.on_enemy_hit.push_back({"SpawnCreep",
                                {{"proc", 0.25}, {"cooldown", 0.4}, {"objectId", std::string("Rocket")},
                                 {"minDamage", 450.0}, {"maxDamage", 550.0}, {"radius", 3.0}}});
  Simulation sim(rifle, {.attack = 25, .dexterity = 46}, At({{0, 3}, {2, 3}, {0, 10}}));
  sim.Advance(10'000);
  CHECK(sim.ProcTotals().casts >= 9);
  CHECK(sim.ProcTotals().casts <= 11);
  CHECK(sim.PerTarget()[1].damage == Approx(499.5 * static_cast<double>(sim.ProcTotals().casts)));
  CHECK(sim.PerTarget()[2].damage == 0);
}

TEST_CASE("a Magic Heal pet's MP lets the ability be cast more often") {
  // 7.7 MP a second at 60 WIS, 52.7 with a level 100 Magic Heal: about 6.8
  // times as many 90 MP casts over a minute, less the full bar both start
  // with.
  rotmg::Character healed = kWis60;
  healed.pet_mp_per_second = rotmg::MagicHealMpPerSecond(100);
  Simulation plain(Sword(), kWis60, At({{0, 3}}), &Ability("War Helm"));
  Simulation pet(Sword(), healed, At({{0, 3}}), &Ability("War Helm"));
  plain.Advance(60'000);
  pet.Advance(60'000);
  CHECK(pet.AbilityTotals().casts > 5 * plain.AbilityTotals().casts);
}
