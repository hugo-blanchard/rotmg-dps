#pragma once

// Scoring builds fast. What a build's items and behaviour-changing
// enchantments decide (the layout) is looked up and applied once; a build
// is then its layout plus the stats its enchantments add, and scoring it
// costs one evaluation of the engine.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "../request.hpp"
#include "model.hpp"
#include "rotmg/build.hpp"

namespace rotmg::search {

/// Weapon and ability DPS weighted as asked, plus a millionth of the total
/// so that ties go to the build with the most damage.
struct Weights {
  double weapon = 1;
  double ability = 1;
};

/// A build's items and the enchantments on each that do more than add
/// stats, sorted.
struct Layout {
  std::array<std::int32_t, kSlots> items{-1, -1, -1, -1};
  std::array<std::vector<std::int32_t>, kSlots> specials;
  auto operator<=>(const Layout&) const = default;
};

/// A build's enchantment stats: flat amounts summed in tenths, and every
/// conversion in the order the engine collects them.
struct EnchantStats {
  std::array<std::int32_t, kStats> tenths{};
  std::vector<Conversion> conversions;
};

/// A layout looked up and applied.
struct PreparedLayout {
  /// The weapon and ability with the layout's enchantments, the target and
  /// assumptions; the character has the base stats, gear and sets but no
  /// enchantment stats.
  Request request;
  /// What conversions count on top of the character: the weapon's and
  /// ability's own stat bonuses, which the evaluation adds later.
  Stats item_bonuses{};
  /// The items' own conversions.
  std::vector<Conversion> own;
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

  /// The character a build's enchantment stats make on the layout, as the
  /// engine works it out: flat stats summed and rounded down, then each
  /// stat's conversions from those totals, summed and rounded down, the
  /// items' own conversions apart from the enchantments'.
  Character Final(const PreparedLayout& layout, const EnchantStats& stats) const;

  /// The build's sustained score.
  double Score(const PreparedLayout& layout, const Character& character) const;

 private:
  const Model* model_;
  Weights weights_;
  std::map<Layout, std::unique_ptr<PreparedLayout>> prepared_;
};

}  // namespace rotmg::search
