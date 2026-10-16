#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "fixture.hpp"
#include "request.hpp"
#include "search/bounded.hpp"
#include "search/enchantments.hpp"
#include "search/model.hpp"
#include "search/score.hpp"

namespace {

using rotmg::search::kSlots;

// The items of a slot type, and no item.
template <typename Item>
std::vector<std::int32_t> OrNone(const std::vector<Item>& list, int slot_type) {
  std::vector<std::int32_t> out = {-1};
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (list[i].slot_type == slot_type) out.push_back(static_cast<std::int32_t>(i));
  }
  return out;
}

}  // namespace

TEST_CASE("the bounded search finds the best build of every armor and ring pair") {
  const rotmg::Dataset d = LoadSmallDataset();
  for (const rotmg::Class& c : d.classes) {
    rotmg::RequestSpec spec;
    spec.class_id = c.id;
    spec.character = {.attack = c.attack.max, .dexterity = c.dexterity.max, .wisdom = c.wisdom.max};
    spec.target.defense = 30;
    const rotmg::search::Model model(d, spec);
    const std::vector<std::int32_t> weapons = OrNone(d.weapons, c.weapon_slot_type);
    const std::vector<std::int32_t> abilities = OrNone(d.abilities, c.ability_slot_type);
    REQUIRE(weapons.size() > 1);
    REQUIRE(abilities.size() > 1);
    rotmg::search::BoundedSettings settings;
    settings.armors = OrNone(d.armors, c.armor_slot_type);
    settings.rings = OrNone(d.rings, c.ring_slot_type);
    for (const rotmg::search::Weights weights : {rotmg::search::Weights{1, 1}, rotmg::search::Weights{1, 0}}) {
      for (const int per_item : {1, 2}) {
        settings.enchant.per_item = per_item;
        double truth = -1;
        for (const std::int32_t armor : settings.armors) {
          for (const std::int32_t ring : settings.rings) {
            rotmg::search::Scorer scorer(model, weights);
            const auto found = rotmg::search::BestEnchantments(model, scorer, {weapons[1], abilities[1], armor, ring},
                                                               settings.enchant, -1);
            if (found) truth = std::max(truth, found->score);
          }
        }
        std::size_t reports = 0;
        settings.progress = [&](const rotmg::search::BoundedProgress& p) {
          CHECK(p.done <= p.pairs);
          ++reports;
        };
        const auto best = rotmg::search::BestBoundedBuild(model, weights, weapons[1], abilities[1], settings);
        REQUIRE(best.has_value());
        CHECK(best->score == truth);
        CHECK(reports > 0);
      }
    }
  }
}
