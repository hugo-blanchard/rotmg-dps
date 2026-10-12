#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rotmg/model.hpp"

// The search for the build with the most DPS. It knows the items and
// enchantments a build can hold; how much DPS one deals is up to the caller.
namespace rotmg {

/// Items by name in the game's slot order (weapon, ability, armor, ring; ""
/// for an empty slot) and the enchantment ids on each.
struct OptimizerBuild {
  std::array<std::string, 4> items;
  std::array<std::vector<std::string>, 4> enchants;
};

struct OptimizerProblem {
  enum class Enchantments { kKeep, kNone, kBest };
  /// The items to try in each slot. A locked slot holds only the start's.
  std::array<std::vector<std::string>, 4> candidates;
  OptimizerBuild start;
  /// Keep the start's enchantments where they still fit, remove them, or
  /// fill each item with the best.
  Enchantments enchantments = Enchantments::kKeep;
  /// Enchantments to fill per item, no more than the item holds.
  int per_item = 2;
  /// Whether enchantments only an artifact or engraving puts on an item count.
  bool artifacts = true;
  std::size_t results = 5;
  /// A build's DPS, or nothing for one the engine refuses.
  std::function<std::optional<double>(const OptimizerBuild&)> score;
};

struct OptimizerResult {
  /// The start's DPS.
  double baseline = 0;
  long evaluated = 0;
  /// The best build for each set of items tried, best first.
  std::vector<std::pair<double, OptimizerBuild>> builds;
};

/// Nothing when the start itself doesn't evaluate.
std::optional<OptimizerResult> SearchBuilds(const Dataset& dataset, const OptimizerProblem& problem);

}  // namespace rotmg
