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

/// The stats the search can ask about, in this order.
inline constexpr std::array<const char*, 8> kOptimizerStats = {"ATT", "DEX", "SPD", "WIS",
                                                                "VIT", "DEF", "MAXHP", "MAXMP"};

struct OptimizerResult {
  /// The start's DPS.
  double baseline = 0;
  long evaluated = 0;
  /// The best build for each set of items tried, best first.
  std::vector<std::pair<double, OptimizerBuild>> builds;
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
  /// Search wider and longer: every candidate gets the full look, and the
  /// search starts again from every ability. For checking the normal search.
  bool thorough = false;
  std::size_t results = 5;
  /// Called every progress_every_s seconds or so while searching, with how
  /// far along the search is (0 to 1, a rough guess) and the best builds so
  /// far, which are worth keeping if the search is cut short.
  std::function<void(double done, const OptimizerResult& so_far)> progress;
  double progress_every_s = 0.25;
  /// A build's DPS with `extra` added to its stats (in kOptimizerStats
  /// order), or nothing for a build the engine refuses.
  std::function<std::optional<double>(const OptimizerBuild&, const std::array<int, 8>& extra)> score;
};

/// Nothing when the start itself doesn't evaluate.
std::optional<OptimizerResult> SearchBuilds(const Dataset& dataset, const OptimizerProblem& problem);

}  // namespace rotmg
