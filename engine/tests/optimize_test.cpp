#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "rotmg/optimize.hpp"

// The search on a small made-up world with a known DPS formula, checked
// against every build there is.
namespace {

using rotmg::Dataset;
using rotmg::Enchantment;
using rotmg::OptimizerBuild;
using rotmg::OptimizerProblem;

constexpr std::array<const char*, 4> kStats = {"ATT", "DEX", "DEF", "WIS"};

Enchantment Stat(const std::string& id, std::vector<std::pair<std::string, double>> amounts,
                 std::vector<std::string> labels, std::vector<std::string> compatible = {"EQUIPMENT"}) {
  Enchantment e;
  e.id = id;
  e.name = id;
  e.compatible = std::move(compatible);
  e.labels = labels;
  e.incompatible_labels = labels;
  for (const auto& [stat, amount] : amounts) {
    rotmg::Mutator m;
    m.tag = "ActivateOnEquip";
    m.text = "IncrementStat";
    m.attributes.params.emplace("stat", stat);
    m.attributes.params.emplace("amount", amount);
    e.mutators.push_back(m);
  }
  return e;
}

// Two weapons, two abilities, an armor and a ring, all taking the same
// stat enchantments; Stone Blade also takes Stone Heart, made for it.
Dataset World() {
  Dataset d;
  const auto item = [](auto& list, const std::string& name, std::vector<std::string> labels) {
    auto& x = list.emplace_back();
    x.id = name;
    x.name = name;
    x.labels = std::move(labels);
    x.enchanting = {4, "Pool"};
  };
  item(d.weapons, "Blade", {"EQUIPMENT", "WEAPON"});
  item(d.weapons, "Stone Blade", {"EQUIPMENT", "WEAPON", "STONE"});
  item(d.abilities, "Battle Cry", {"EQUIPMENT", "ABILITY"});
  item(d.abilities, "Rampart", {"EQUIPMENT", "ABILITY"});
  item(d.armors, "Mail", {"EQUIPMENT", "ARMOR"});
  item(d.rings, "Band", {"EQUIPMENT", "RING"});
  d.enchantments = {
      Stat("Attack", {{"ATT", 8}}, {"SINGLESTAT"}),
      Stat("Attack_Small", {{"ATT", 4}}, {"SINGLESTAT"}),
      Stat("Dexterity", {{"DEX", 8}}, {"SINGLESTAT"}),
      Stat("Defense", {{"DEF", 8}}, {"SINGLESTAT"}),
      Stat("Dual", {{"ATT", 5}, {"DEF", 5}}, {"DUALSTAT"}),
      Stat("Stone_Heart", {{"DEF", 12}}, {"AWAKENED"}, {"WEAPON", "STONE"}),
  };
  for (const Enchantment& e : d.enchantments) {
    d.enchantment_pools["Pool"].insert(e.id);
  }
  return d;
}

// The weapon's DPS grows with ATT and DEX; Battle Cry adds a little with
// ATT; Rampart does nothing below 50 DEF and grows fast above it. Stone
// Blade hits a bit softer than Blade.
std::optional<double> Dps(const Dataset& d, const OptimizerBuild& b, const std::array<int, 8>& extra) {
  if (b.items[0].empty()) {
    return std::nullopt;
  }
  std::map<std::string, double> stats = {{"ATT", 50}, {"DEX", 50}, {"DEF", 10}, {"WIS", 50}};
  for (std::size_t i = 0; i < rotmg::kOptimizerStats.size(); ++i) {
    stats[rotmg::kOptimizerStats[i]] += extra[i];
  }
  if (b.items[2] == "Mail") {
    stats["DEF"] += 10;
  }
  for (const auto& list : b.enchants) {
    for (const std::string& id : list) {
      const auto e = std::ranges::find(d.enchantments, id, &Enchantment::id);
      for (const rotmg::Mutator& m : e->mutators) {
        stats[m.attributes.Text("stat")] += *m.attributes.Number("amount");
      }
    }
  }
  const double weapon = (stats["ATT"] / 10 + 1) * (stats["DEX"] / 10 + 1) * (b.items[0] == "Blade" ? 100 : 90);
  const double ability = b.items[1] == "Rampart" ? 8 * std::pow(std::max(0.0, stats["DEF"] - 50), 2)
                         : b.items[1] == "Battle Cry" ? 50 + 2 * stats["ATT"]
                                                      : 0;
  return weapon + ability;
}

// Every legal set of up to `per_item` enchantments on an item.
std::vector<std::vector<std::string>> Choices(const Dataset& d, const std::string& item, int per_item) {
  std::vector<const Enchantment*> fits;
  for (const Enchantment& e : d.enchantments) {
    const bool own = std::ranges::find(e.compatible, "STONE") != e.compatible.end();
    if (!own || item == "Stone Blade") {
      fits.push_back(&e);
    }
  }
  std::vector<std::vector<std::string>> out = {{}};
  for (int k = 0; k < per_item; ++k) {
    std::vector<std::vector<std::string>> grown = out;
    for (const auto& set : out) {
      if (std::cmp_less(set.size(), k)) {
        continue;
      }
      for (const Enchantment* e : fits) {
        const bool clash = std::ranges::any_of(set, [&](const std::string& id) {
          const auto o = std::ranges::find(d.enchantments, id, &Enchantment::id);
          return id <= e->id || o->labels == e->labels;
        });
        if (!clash) {
          auto next = set;
          next.push_back(e->id);
          grown.push_back(next);
        }
      }
    }
    out = grown;
  }
  return out;
}

double BestByBruteForce(const Dataset& d, int per_item, OptimizerBuild* best_build) {
  double best = 0;
  for (const char* weapon : {"Blade", "Stone Blade"}) {
    for (const char* ability : {"Battle Cry", "Rampart"}) {
      const auto w = Choices(d, weapon, per_item);
      const auto a = Choices(d, ability, per_item);
      const auto r = Choices(d, "Mail", per_item);
      const auto g = Choices(d, "Band", per_item);
      for (const auto& ew : w)
        for (const auto& ea : a)
          for (const auto& er : r)
            for (const auto& eg : g) {
              const OptimizerBuild b{{weapon, ability, "Mail", "Band"}, {ew, ea, er, eg}};
              if (const double dps = *Dps(d, b, {}); dps > best) {
                best = dps;
                *best_build = b;
              }
            }
    }
  }
  return best;
}

OptimizerProblem Problem(const Dataset& d, int per_item) {
  OptimizerProblem p;
  p.candidates = {{{"Blade", "Stone Blade"}, {"Battle Cry", "Rampart"}, {"Mail"}, {"Band"}}};
  // Everything on ATT. Swapping Battle Cry for Rampart alone loses DPS
  // here, and so does Stone Blade alone: only together, with every item's
  // enchantments moved to DEF, do they win.
  p.start.items = {"Blade", "Battle Cry", "Mail", "Band"};
  p.start.enchants = {{{"Attack"}, {"Attack"}, {"Attack"}, {"Attack"}}};
  p.enchantments = OptimizerProblem::Enchantments::kBest;
  p.per_item = per_item;
  p.score = [&d](const OptimizerBuild& b, const std::array<int, 8>& extra) { return Dps(d, b, extra); };
  return p;
}

}  // namespace

TEST_CASE("the search finds a build that only works around one ability") {
  const Dataset d = World();
  OptimizerBuild truth;
  const double best = BestByBruteForce(d, 2, &truth);
  // The best build stacks DEF for Rampart, with Stone Blade's awakening.
  CHECK(truth.items[0] == "Stone Blade");
  CHECK(truth.items[1] == "Rampart");
  const auto found = rotmg::SearchBuilds(d, Problem(d, 2));
  REQUIRE(found.has_value());
  CHECK(found->builds[0].first == Catch::Approx(best));
  CHECK(found->builds[0].second.items == truth.items);
}

TEST_CASE("the search matches every build tried one by one") {
  const Dataset d = World();
  for (const int per_item : {1, 2}) {
    OptimizerBuild truth;
    const double best = BestByBruteForce(d, per_item, &truth);
    const auto found = rotmg::SearchBuilds(d, Problem(d, per_item));
    REQUIRE(found.has_value());
    CHECK(found->builds[0].first == Catch::Approx(best));
  }
}

TEST_CASE("the search's start stays its baseline and nothing found is worse") {
  const Dataset d = World();
  const OptimizerProblem p = Problem(d, 2);
  const auto found = rotmg::SearchBuilds(d, p);
  REQUIRE(found.has_value());
  CHECK(found->baseline == Catch::Approx(*Dps(d, p.start, {})));
  CHECK(found->builds[0].first >= found->baseline);
}
