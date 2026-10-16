#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "fixture.hpp"
#include "request.hpp"
#include "rotmg/build.hpp"
#include "rotmg/enchant.hpp"
#include "search/model.hpp"
#include "search/score.hpp"

namespace {

using rotmg::search::Build;
using rotmg::search::kSlots;

// Up to the item's slots of enchantments it can take, none clashing.
template <typename Item>
std::vector<std::int32_t> RandomEnchantments(const rotmg::Dataset& d, const Item& item, std::mt19937& rng) {
  std::vector<std::int32_t> fits;
  for (std::size_t i = 0; i < d.enchantments.size(); ++i) {
    if (rotmg::HowObtained(d.enchantments[i], item, d) != rotmg::Obtained::kNever) {
      fits.push_back(static_cast<std::int32_t>(i));
    }
  }
  std::ranges::shuffle(fits, rng);
  const int slots = rotmg::EnchantmentSlots(item.enchanting, d);
  std::vector<std::int32_t> out;
  for (const std::int32_t e : fits) {
    if (std::cmp_greater_equal(out.size(), slots)) {
      break;
    }
    const auto clash = [&](std::int32_t o) {
      return rotmg::Conflict(d.enchantments[static_cast<std::size_t>(o)], d.enchantments[static_cast<std::size_t>(e)]);
    };
    if (std::ranges::none_of(out, clash)) {
      out.push_back(e);
    }
  }
  return out;
}

// Indices of the items in `list` of the given slot type.
template <typename Item>
std::vector<std::int32_t> OfType(const std::vector<Item>& list, int slot_type) {
  std::vector<std::int32_t> out;
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (list[i].slot_type == slot_type) {
      out.push_back(static_cast<std::int32_t>(i));
    }
  }
  return out;
}

}  // namespace

TEST_CASE("the search's scorer gives the engine's character and DPS for any build") {
  const rotmg::Dataset d = LoadSmallDataset();
  std::mt19937 rng(7);
  int compared = 0;
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
    rotmg::search::Scorer scorer(model, {});
    const std::array<std::vector<std::int32_t>, kSlots> items = {
        OfType(d.weapons, c.weapon_slot_type), OfType(d.abilities, c.ability_slot_type),
        OfType(d.armors, c.armor_slot_type), OfType(d.rings, c.ring_slot_type)};
    for (int trial = 0; trial < 300; ++trial) {
      Build b;
      for (std::size_t s = 0; s < kSlots; ++s) {
        const bool empty = items[s].empty() || (s != 0 && rng() % 5 == 0);
        b.items[s] = empty ? -1 : items[s][rng() % items[s].size()];
      }
      if (b.items[0] < 0) {
        continue;
      }
      const auto at = [](const auto& list, std::int32_t i) { return list[static_cast<std::size_t>(i)]; };
      b.enchants[0] = RandomEnchantments(d, at(d.weapons, b.items[0]), rng);
      if (b.items[1] >= 0) b.enchants[1] = RandomEnchantments(d, at(d.abilities, b.items[1]), rng);
      if (b.items[2] >= 0) b.enchants[2] = RandomEnchantments(d, at(d.armors, b.items[2]), rng);
      if (b.items[3] >= 0) b.enchants[3] = RandomEnchantments(d, at(d.rings, b.items[3]), rng);

      rotmg::RequestSpec full = spec;
      full.weapon = model.ItemName(0, b.items[0]);
      const auto named = [&](std::size_t s) {
        return b.items[s] < 0 ? std::nullopt : std::optional<std::string>(model.ItemName(s, b.items[s]));
      };
      full.ability = named(1);
      full.armor = named(2);
      full.ring = named(3);
      const auto ids = [&](std::size_t s) {
        std::vector<std::string> out;
        for (const std::int32_t e : b.enchants[s]) out.push_back(model.enchantment(e).id);
        return out;
      };
      full.enchantments = ids(0);
      full.gear_enchantments.emplace();
      (*full.gear_enchantments)["ability"] = ids(1);
      (*full.gear_enchantments)["armor"] = ids(2);
      (*full.gear_enchantments)["ring"] = ids(3);
      const rotmg::Request expected = rotmg::Assemble(full, d);

      const rotmg::search::PreparedLayout* layout = scorer.Prepare(rotmg::search::LayoutOf(model, b));
      REQUIRE(layout != nullptr);
      const rotmg::Character mine = scorer.Final(*layout, rotmg::search::StatsOf(model, b));
      const rotmg::Character& theirs = expected.character;
      CHECK(mine.attack == theirs.attack);
      CHECK(mine.dexterity == theirs.dexterity);
      CHECK(mine.speed == theirs.speed);
      CHECK(mine.wisdom == theirs.wisdom);
      CHECK(mine.vitality == theirs.vitality);
      CHECK(mine.defense == theirs.defense);
      CHECK(mine.max_hit_points == theirs.max_hit_points);
      CHECK(mine.max_magic_points == theirs.max_magic_points);
      CHECK(mine.worn == theirs.worn);
      const auto dps = [](const rotmg::Request& r, const rotmg::Character& ch) {
        return rotmg::EvaluateBuild(*r.weapon, r.ability, ch, r.target, r.assumptions).dps;
      };
      CHECK(dps(layout->request, mine) == dps(expected, theirs));
      const rotmg::BuildBreakdown full_eval =
          rotmg::EvaluateBuild(*expected.weapon, expected.ability, theirs, expected.target, expected.assumptions);
      const rotmg::BuildNumbers numbers =
          rotmg::EvaluateBuildNumbers(*expected.weapon, expected.ability, theirs, expected.target, expected.assumptions);
      CHECK(numbers.dps == full_eval.dps);
      CHECK(numbers.weapon_dps == full_eval.weapon_dps);
      CHECK(numbers.ability_dps == (full_eval.ability ? full_eval.ability->dps : 0.0));
      ++compared;
    }
  }
  CHECK(compared > 300);
}

TEST_CASE("items prepared once take any enchantments like a request prepared whole") {
  const rotmg::Dataset d = LoadSmallDataset();
  std::mt19937 rng(11);
  int compared = 0;
  for (const rotmg::Class& c : d.classes) {
    rotmg::RequestSpec spec;
    spec.class_id = c.id;
    spec.character = {.attack = c.attack.max, .dexterity = c.dexterity.max, .wisdom = c.wisdom.max};
    spec.target.defense = 30;
    const std::array<std::vector<std::int32_t>, kSlots> items = {
        OfType(d.weapons, c.weapon_slot_type), OfType(d.abilities, c.ability_slot_type),
        OfType(d.armors, c.armor_slot_type), OfType(d.rings, c.ring_slot_type)};
    for (int trial = 0; trial < 40; ++trial) {
      std::array<std::int32_t, kSlots> pick{};
      for (std::size_t s = 0; s < kSlots; ++s) {
        pick[s] = items[s].empty() || (s != 0 && rng() % 4 == 0) ? -1 : items[s][rng() % items[s].size()];
      }
      if (pick[0] < 0) {
        continue;
      }
      const auto at = [](const auto& list, std::int32_t i) { return list[static_cast<std::size_t>(i)]; };
      rotmg::RequestSpec bare = spec;
      bare.weapon = at(d.weapons, pick[0]).name;
      if (pick[1] >= 0) bare.ability = at(d.abilities, pick[1]).name;
      if (pick[2] >= 0) bare.armor = at(d.armors, pick[2]).name;
      if (pick[3] >= 0) bare.ring = at(d.rings, pick[3]).name;
      const rotmg::PreparedItems prepared = rotmg::PrepareItems(bare, d);
      // The same prepared items, with several sets of enchantments.
      for (int round = 0; round < 3; ++round) {
        const auto ids = [&](const auto& list, std::int32_t i) {
          std::vector<std::string> out;
          if (i >= 0) {
            for (const std::int32_t e : RandomEnchantments(d, at(list, i), rng)) {
              out.push_back(d.enchantments[static_cast<std::size_t>(e)].id);
            }
          }
          return out;
        };
        rotmg::RequestSpec full = bare;
        full.enchantments = ids(d.weapons, pick[0]);
        full.gear_enchantments.emplace();
        (*full.gear_enchantments)["ability"] = ids(d.abilities, pick[1]);
        (*full.gear_enchantments)["armor"] = ids(d.armors, pick[2]);
        (*full.gear_enchantments)["ring"] = ids(d.rings, pick[3]);
        const rotmg::Request whole = rotmg::Assemble(full, d);
        const rotmg::Request reused = rotmg::Finish(rotmg::PrepareEnchantments(prepared, full, d), full.character);
        CHECK(reused.character.attack == whole.character.attack);
        CHECK(reused.character.dexterity == whole.character.dexterity);
        CHECK(reused.weapon->on_shoot.size() == whole.weapon->on_shoot.size());
        CHECK((reused.ability == nullptr) == (whole.ability == nullptr));
        if (whole.ability != nullptr) {
          CHECK(reused.ability->mp_cost == whole.ability->mp_cost);
          CHECK(reused.ability->on_ability.size() == whole.ability->on_ability.size());
        }
        const auto dps = [](const rotmg::Request& r) {
          return rotmg::EvaluateBuild(*r.weapon, r.ability, r.character, r.target, r.assumptions).dps;
        };
        CHECK(dps(reused) == dps(whole));
        ++compared;
      }
    }
  }
  CHECK(compared > 50);
}
