#pragma once

// Scoring builds fast. What a build's items and behaviour-changing
// enchantments decide (the layout) is looked up and applied once; a build
// is then its layout plus the stats its enchantments add, and scoring it
// costs one evaluation of the engine.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include "../request.hpp"
#include "model.hpp"
#include "rotmg/build.hpp"

namespace rotmg::search {

/// Weapon and ability DPS weighted as asked, plus a millionth of the total
/// so that ties go to the build with the most damage.
using Weights = DpsWeights;

/// A build's items and the enchantments on each that do more than add
/// stats, sorted.
struct Layout {
  std::array<std::int32_t, kSlots> items{-1, -1, -1, -1};
  std::array<std::vector<std::int32_t>, kSlots> specials;
  auto operator<=>(const Layout&) const = default;
};

/// A layout looked up and applied.
struct PreparedLayout {
  /// The weapon and ability with the layout's enchantments, the target and
  /// assumptions; the character's stats come from FinalStats.
  Request request;
  /// The base stats, gear and sets, and what else decides the final stats
  /// besides the build's enchantments.
  StatBase stats;
};

/// The build's layout.
Layout LayoutOf(const Model& model, const Build& build);
/// The build's enchantment stats, ring, armor, ability, then weapon, as the
/// engine collects them.
EnchantStats StatsOf(const Model& model, const Build& build);

class Scorer {
 public:
  Scorer(const Model& model, Weights weights) : model_(&model), weights_(weights) {}

  /// The layout prepared, or nullptr when the engine rejects it (an
  /// enchantment that doesn't fit, an unknown item). Kept for later calls.
  const PreparedLayout* Prepare(const Layout& layout);

  /// The character a build's enchantment stats make on the layout, through
  /// FinalStats as for any request.
  Character Final(const PreparedLayout& layout, const EnchantStats& stats) const;
  /// The same from flat sums in tenths and the conversions, without
  /// gathering them first.
  Character Final(const PreparedLayout& layout, const Tenths& tenths, std::span<const Conversion> conversions) const;
  /// Just the stats, in kOptimizerStats order: no allocation, for loops
  /// over many sums.
  Stats FinalStats(const PreparedLayout& layout, const Tenths& tenths, std::span<const Conversion> conversions) const {
    return rotmg::FinalStats(layout.stats, tenths, conversions);
  }
  /// The layout's character with these stats.
  Character WithStats(const PreparedLayout& layout, const Stats& stats) const;

  /// The build's sustained score.
  double Score(const PreparedLayout& layout, const Character& character) const;
  /// The score without the millionth that breaks ties: what the weights
  /// ask for.
  double Weighted(const PreparedLayout& layout, const Character& character) const;

  const Weights& weights() const { return weights_; }

 private:
  const Model* model_;
  Weights weights_;
  std::map<Layout, std::unique_ptr<PreparedLayout>> prepared_;
};

}  // namespace rotmg::search
