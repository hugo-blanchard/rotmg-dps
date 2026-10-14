#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <utility>
#include <vector>

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
