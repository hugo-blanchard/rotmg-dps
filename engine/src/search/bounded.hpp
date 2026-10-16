#pragma once

// The best build for a fixed weapon and ability among chosen armors and
// rings, with the best enchantments on all four: an exact search over a
// bounded set of items.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "enchantments.hpp"
#include "model.hpp"
#include "score.hpp"

namespace rotmg::search {

struct BoundedProgress {
  /// Armor and ring pairs searched, out of `pairs`, and the share of the
  /// current one done.
  std::size_t done = 0;
  std::size_t pairs = 0;
  double within = 0;
  /// The best build so far.
  std::optional<EnchantResult> best;
};

struct BoundedSettings {
  EnchantSettings enchant;
  /// The armors and rings to choose from, by index; -1 is no item.
  std::vector<std::int32_t> armors;
  std::vector<std::int32_t> rings;
  /// Called now and then on the thread that started the search.
  std::function<void(const BoundedProgress&)> progress;
};

/// The best build with this weapon and ability and one of the armors and
/// one of the rings, or nothing when none evaluates. Exact as
/// BestEnchantments is, for every pair.
///
/// Pairs are searched best first by their score without enchantments, each
/// only for builds that beat the best so far, so most end early.
std::optional<EnchantResult> BestBoundedBuild(const Model& model, Weights weights, std::int32_t weapon,
                                              std::int32_t ability, const BoundedSettings& settings);

}  // namespace rotmg::search
