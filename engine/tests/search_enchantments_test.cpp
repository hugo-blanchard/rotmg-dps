#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "fixture.hpp"
#include "request.hpp"
#include "rotmg/enchant.hpp"
#include "search/enchantments.hpp"
#include "search/model.hpp"
#include "search/score.hpp"

namespace {

using rotmg::search::Build;
using rotmg::search::kSlots;

// Every clash-free set of up to `most` enchantments the item can take.
template <typename Item>
std::vector<std::vector<std::int32_t>> EverySet(const rotmg::Dataset& d, const Item& item, int per_item) {
  std::vector<std::int32_t> fits;
  for (std::size_t i = 0; i < d.enchantments.size(); ++i) {
    if (rotmg::HowObtained(d.enchantments[i], item, d) != rotmg::Obtained::kNever) {
      fits.push_back(static_cast<std::int32_t>(i));
    }
  }
  const int most = std::min(per_item, rotmg::EnchantmentSlots(item.enchanting, d));
  std::vector<std::vector<std::int32_t>> out;
  std::vector<std::int32_t> set;
  std::function<void(std::size_t)> grow = [&](std::size_t from) {
    out.push_back(set);
    if (std::cmp_greater_equal(set.size(), most)) return;
    for (std::size_t k = from; k < fits.size(); ++k) {
      const auto& e = d.enchantments[static_cast<std::size_t>(fits[k])];
      const bool clash = std::ranges::any_of(set, [&](std::int32_t o) {
        return rotmg::Conflict(d.enchantments[static_cast<std::size_t>(o)], e);
      });
      if (clash) continue;
      set.push_back(fits[k]);
      grow(k + 1);
      set.pop_back();
    }
  };
  grow(0);
  return out;
}

template <typename Item>
std::vector<std::int32_t> OfType(const std::vector<Item>& list, int slot_type) {
  std::vector<std::int32_t> out;
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (list[i].slot_type == slot_type) out.push_back(static_cast<std::int32_t>(i));
  }
  return out;
}

// The best score over every legal build on these items, scored as the
// search scores them: with the tie-break, and without.
std::pair<double, double> BruteForce(const rotmg::Dataset& d, const rotmg::search::Model& model, rotmg::search::Scorer& scorer,
                  const std::array<std::int32_t, kSlots>& items, int per_item) {
  std::array<std::vector<std::vector<std::int32_t>>, kSlots> sets;
  const auto at = [](const auto& list, std::int32_t i) { return list[static_cast<std::size_t>(i)]; };
  sets[0] = EverySet(d, at(d.weapons, items[0]), per_item);
  sets[1] = items[1] < 0 ? std::vector<std::vector<std::int32_t>>{{}} : EverySet(d, at(d.abilities, items[1]), per_item);
  sets[2] = items[2] < 0 ? std::vector<std::vector<std::int32_t>>{{}} : EverySet(d, at(d.armors, items[2]), per_item);
  sets[3] = items[3] < 0 ? std::vector<std::vector<std::int32_t>>{{}} : EverySet(d, at(d.rings, items[3]), per_item);
  double best = -1;
  double best_weighted = -1;
  Build b;
  b.items = items;
  for (const auto& w : sets[0]) {
    for (const auto& a : sets[1]) {
      for (const auto& r : sets[2]) {
        for (const auto& g : sets[3]) {
          b.enchants = {w, a, r, g};
          if (const auto* layout = scorer.Prepare(rotmg::search::LayoutOf(model, b))) {
            const rotmg::Character c = scorer.Final(*layout, rotmg::search::StatsOf(model, b));
            best = std::max(best, scorer.Score(*layout, c));
            best_weighted = std::max(best_weighted, scorer.Weighted(*layout, c));
          }
        }
      }
    }
  }
  return {best, best_weighted};
}

}  // namespace

TEST_CASE("the enchantment search finds the best build every build tried one by one finds") {
  const rotmg::Dataset d = LoadSmallDataset();
  std::mt19937 rng(11);
  for (const rotmg::Class& c : d.classes) {
    rotmg::RequestSpec spec;
    spec.class_id = c.id;
    spec.character = {.attack = c.attack.max,
                      .dexterity = c.dexterity.max,
                      .wisdom = c.wisdom.max,
                      .vitality = c.vitality.max,
                      .defense = c.defense.max,
                      .speed = c.speed.max,
                      .max_hit_points = c.max_hit_points.max,
                      .max_magic_points = c.max_magic_points.max};
    spec.target.defense = 30;
    spec.assumptions.distance = 4.0;
    const rotmg::search::Model model(d, spec);
    const std::array<std::vector<std::int32_t>, kSlots> items = {
        OfType(d.weapons, c.weapon_slot_type), OfType(d.abilities, c.ability_slot_type),
        OfType(d.armors, c.armor_slot_type), OfType(d.rings, c.ring_slot_type)};
    for (const rotmg::search::Weights weights :
         {rotmg::search::Weights{1, 1}, rotmg::search::Weights{1, 0}, rotmg::search::Weights{0, 1}}) {
      for (int trial = 0; trial < 6; ++trial) {
        std::array<std::int32_t, kSlots> pick{};
        for (std::size_t s = 0; s < kSlots; ++s) {
          pick[s] = items[s].empty() ? -1 : items[s][rng() % items[s].size()];
        }
        if (pick[0] < 0) continue;
        // One enchantment on each item, then two on the weapon and ring.
        for (const int per_item : {1, 2}) {
          std::array<std::int32_t, kSlots> used = pick;
          if (per_item == 2) used[1] = used[2] = -1;
          rotmg::search::Scorer scorer(model, weights);
          const auto [truth, truth_weighted] = BruteForce(d, model, scorer, used, per_item);
          rotmg::search::SearchCounts counts;
          const auto found = rotmg::search::BestEnchantments(model, scorer, used, {.per_item = per_item}, -1, &counts);
          REQUIRE(found.has_value());
          CHECK(found->score <= truth * (1 + 1e-12));
          if (weights.weapon == 0 || weights.ability == 0) {
            // Exact up to the tie-break: the weighted part is the best there
            // is, whatever the millionth of the total.
            const auto* layout = scorer.Prepare(rotmg::search::LayoutOf(model, found->build));
            REQUIRE(layout != nullptr);
            const double weighted =
                scorer.Weighted(*layout, scorer.Final(*layout, rotmg::search::StatsOf(model, found->build)));
            CHECK(weighted == Catch::Approx(truth_weighted).epsilon(1e-12));
          } else {
            CHECK(found->score == Catch::Approx(truth).epsilon(1e-12));
          }
        }
      }
    }
  }
}

namespace {

// The fixture with more enchantments, as JSON.
rotmg::Dataset WithEnchantments(const std::vector<nlohmann::json>& more) {
  nlohmann::json d = nlohmann::json::parse(ReadSmallDatasetText());
  for (const nlohmann::json& e : more) d["enchantments"].push_back(e);
  return rotmg::ParseDataset(d.dump());
}

nlohmann::json AttackBoost(const std::string& id, int type, int amount, int duration, int cooldown,
                           const std::string& family) {
  return {{"type", type},
          {"id", id},
          {"compatible", {"EQUIPMENT"}},
          {"incompatible", {"ARMOR", "ABILITY"}},
          {"labels", {"ONSHOOT", family}},
          {"incompatibleLabels", {family}},
          {"mutators",
           {{{"tag", "AddOnPlayerShootActivate"},
             {"value", "StatBoostSelf"},
             {"params",
              {{"amount", amount},
               {"channel", "noStack"},
               {"cooldown", cooldown},
               {"duration", duration},
               {"proc", 0.03},
               {"stat", "ATT"}}}}}}};
}

std::int32_t EnchantmentIndex(const rotmg::Dataset& d, const std::string& id) {
  const auto it = std::ranges::find(d.enchantments, id, &rotmg::Enchantment::id);
  REQUIRE(it != d.enchantments.end());
  return static_cast<std::int32_t>(it - d.enchantments.begin());
}

std::int32_t NamedIndex(const auto& list, const std::string& name) {
  const auto it = std::ranges::find(list, name, [](const auto& x) { return x.name; });
  REQUIRE(it != list.end());
  return static_cast<std::int32_t>(it - list.begin());
}

rotmg::RequestSpec Swordsman(const rotmg::Dataset& d, double distance) {
  const rotmg::Class& c = *std::ranges::find(d.classes, std::string("Swordsman"), &rotmg::Class::id);
  rotmg::RequestSpec spec;
  spec.class_id = c.id;
  spec.character = {.attack = c.attack.max, .dexterity = c.dexterity.max, .wisdom = c.wisdom.max};
  spec.target.defense = 10;
  spec.assumptions.distance = distance;
  return spec;
}

// The search's best against every build's, on these items.
void MatchesBruteForce(const rotmg::Dataset& d, const rotmg::RequestSpec& spec,
                       const std::array<std::int32_t, kSlots>& items, int per_item) {
  const rotmg::search::Model model(d, spec);
  rotmg::search::Scorer scorer(model, {1, 1});
  const auto [truth, truth_weighted] = BruteForce(d, model, scorer, items, per_item);
  const auto found = rotmg::search::BestEnchantments(model, scorer, items, {.per_item = per_item}, -1);
  REQUIRE(found.has_value());
  CHECK(found->score == Catch::Approx(truth).epsilon(1e-12));
}

}  // namespace

TEST_CASE("a stronger boost that doesn't stack replaces a weaker one unless a rival ranks between them") {
  // Tier III and IV differ only in amount. A rival on the ring with tier
  // III's amount but a longer boost and shorter cooldown ranks between them:
  // with tier III on the weapon the rival counts, with tier IV, tier IV
  // does, and it's the weaker of the two.
  const auto tier3 = AttackBoost("Attack_Boost_3", 901, 7, 6, 15, "PROCATTACK");
  const auto rival = AttackBoost("Quick_Attack_Boost", 902, 7, 7, 5, "QUICKATTACK");
  for (const bool with_rival : {false, true}) {
    const rotmg::Dataset d = with_rival ? WithEnchantments({tier3, rival}) : WithEnchantments({tier3});
    const rotmg::RequestSpec spec = Swordsman(d, 2);
    const rotmg::search::Model model(d, spec);
    rotmg::search::Scorer scorer(model, {1, 1});
    const std::array<std::int32_t, kSlots> items = {NamedIndex(d.weapons, "Plain Sword"), -1, -1,
                                                    NamedIndex(d.rings, "Plain Ring")};
    const auto assumed = rotmg::search::AssumptionsFor(model, scorer, items, {.per_item = 1});
    const std::pair replaced{EnchantmentIndex(d, "Attack_Boost_3"), EnchantmentIndex(d, "OnShoot_Attack_Bonus_4")};
    INFO("with the rival: " << with_rival);
    CHECK((std::ranges::find(assumed.replaced[0], replaced) != assumed.replaced[0].end()) == !with_rival);
    MatchesBruteForce(d, spec, items, 1);
  }
}

TEST_CASE("projectile speed is ignored only where it can't change the hits") {
  // Plain Sword's shots reach 3.5 tiles. A target 4.3 tiles away with a
  // 0.5 tile radius is hit only with 20% more speed; one 2 tiles away is
  // hit either way.
  const rotmg::Dataset d = WithEnchantments({{{"type", 903},
                                              {"id", "Speed_Test"},
                                              {"compatible", {"WEAPON"}},
                                              {"labels", {"PROJSPEED"}},
                                              {"incompatibleLabels", {"PROJSPEED"}},
                                              {"mutators", {{{"tag", "MultiplySpeed"}, {"value", 1.2}}}}}});
  const std::array<std::int32_t, kSlots> items = {NamedIndex(d.weapons, "Plain Sword"), -1, -1, -1};
  for (const double distance : {2.0, 4.3}) {
    const rotmg::RequestSpec spec = Swordsman(d, distance);
    const rotmg::search::Model model(d, spec);
    rotmg::search::Scorer scorer(model, {1, 1});
    const auto assumed = rotmg::search::AssumptionsFor(model, scorer, items, {.per_item = 2});
    const bool ignored = std::ranges::find(assumed.ignored[0], EnchantmentIndex(d, "Speed_Test")) != assumed.ignored[0].end();
    INFO("distance " << distance);
    CHECK(ignored == (distance < 3));
    MatchesBruteForce(d, spec, items, 2);
  }
}

namespace {

// Plain Ring with its own effect `stat` +25% of itself and `other` a share
// of `stat`, like Faded Heart Scarab (DEX, ATT, -15%) and Fractured Hannya
// (ATT, DEX, -15%).
nlohmann::json TradeRing(const nlohmann::json& d, const std::string& name, int type, const std::string& stat,
                         const std::string& other, int penalty) {
  nlohmann::json ring = *std::ranges::find_if(d["rings"], [](const auto& r) { return r["name"] == "Plain Ring"; });
  ring["type"] = type;
  ring["id"] = name;
  ring["name"] = name;
  ring["onEquip"] = {
      {{"type", "IncrementStatRelative"}, {"params", {{"amount", 25}, {"stat", stat}, {"statRelativeTo", stat}}}},
      {{"type", "IncrementStatRelative"}, {"params", {{"amount", penalty}, {"stat", other}, {"statRelativeTo", stat}}}}};
  return ring;
}

// An enchantment any item rolls that adds flat DEX and ATT and stacks with
// everything.
nlohmann::json FlatStats(const std::string& id, int type, double dex, double att) {
  nlohmann::json mutators = nlohmann::json::array();
  for (const auto& [stat, amount] : {std::pair{"DEX", dex}, std::pair{"ATT", att}}) {
    if (amount == 0) continue;
    mutators.push_back(
        {{"tag", "ActivateOnEquip"}, {"value", "IncrementStat"}, {"params", {{"stat", stat}, {"amount", amount}}}});
  }
  return {{"type", type},      {"id", id},          {"name", id}, {"compatible", {"EQUIPMENT"}},
          {"labels", {"STAT", id}}, {"mutators", mutators}};
}

}  // namespace

TEST_CASE("a ring that trades one read stat for another still gets its best enchantments") {
  // More DEX with the DEX ring is more DEX but less ATT; the fixture's
  // Dexterity To Attack Bonus converts DEX into ATT the other way. With the
  // harsher rings and more ways to add DEX and ATT, the best builds don't
  // take all the DEX they can.
  nlohmann::json json = nlohmann::json::parse(ReadSmallDatasetText());
  for (const auto& [name, type, stat, other, penalty] :
       std::vector<std::tuple<std::string, int, std::string, std::string, int>>{
           {"DEX Trade Ring", 904, "DEX", "ATT", -15},
           {"ATT Trade Ring", 905, "ATT", "DEX", -15},
           {"Harsh DEX Trade Ring", 906, "DEX", "ATT", -80},
           {"Harsh ATT Trade Ring", 907, "ATT", "DEX", -80}}) {
    json["rings"].push_back(TradeRing(json, name, type, stat, other, penalty));
  }
  int type = 910;
  for (const auto& [id, dex, att] : std::vector<std::tuple<std::string, double, double>>{
           {"Big_Dexterity", 4.6, 0}, {"Dexterity_Some_Attack", 3, 0.5}, {"Big_Attack", 0, 2.6}, {"Even", 1.2, 1.6}}) {
    json["enchantments"].push_back(FlatStats(id, type++, dex, att));
    json["enchantmentPools"][0]["enchantments"].push_back(id);
  }
  const rotmg::Dataset d = rotmg::ParseDataset(json.dump());
  for (const rotmg::Class& c : d.classes) {
    rotmg::RequestSpec spec;
    spec.class_id = c.id;
    spec.character = {.attack = c.attack.max,
                      .dexterity = c.dexterity.max,
                      .wisdom = c.wisdom.max,
                      .vitality = c.vitality.max,
                      .defense = c.defense.max,
                      .speed = c.speed.max,
                      .max_hit_points = c.max_hit_points.max,
                      .max_magic_points = c.max_magic_points.max};
    spec.target.defense = 30;
    spec.assumptions.distance = 2.0;
    // The class's first item of each kind, or none.
    const auto first = [](const std::vector<std::int32_t>& list) { return list.empty() ? -1 : list.front(); };
    const std::int32_t weapon = first(OfType(d.weapons, c.weapon_slot_type));
    const std::int32_t ability = first(OfType(d.abilities, c.ability_slot_type));
    const std::int32_t armor = first(OfType(d.armors, c.armor_slot_type));
    for (const std::string ring : {"DEX Trade Ring", "ATT Trade Ring", "Harsh DEX Trade Ring", "Harsh ATT Trade Ring"}) {
      INFO(c.id << " with " << ring);
      const std::int32_t r = NamedIndex(d.rings, ring);
      MatchesBruteForce(d, spec, {weapon, ability, armor, r}, 1);
      MatchesBruteForce(d, spec, {weapon, -1, -1, r}, 2);
    }
  }
}
