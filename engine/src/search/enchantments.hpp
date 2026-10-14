#pragma once

// The best enchantments for a fixed set of items, by a search that proves
// its answer rather than hoping to stumble on it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "model.hpp"
#include "score.hpp"

namespace rotmg::search {

struct EnchantSettings {
  /// Most enchantments per item; each item also holds no more than its
  /// slots.
  int per_item = 4;
  /// Whether enchantments that only an artifact or an engraving puts on an
  /// item count.
  bool artifacts = true;
};

struct EnchantResult {
  double score = 0;
  Build build;
};

/// What the search did, for tests and for showing its work.
struct SearchCounts {
  std::size_t layouts = 0;
  std::size_t layouts_cut = 0;
  std::size_t profiles = 0;
  std::size_t assignments = 0;
  std::size_t finals = 0;
  std::size_t scored = 0;
};

/// The best enchantments for the items in `items` (-1 for an empty slot;
/// the weapon must be there), or nothing when no build scores above
/// `incumbent`.
///
/// Exact as long as more of a stat the score reads never lowers it and a
/// stronger on-shoot boost never does either, both property-tested. With a
/// weight at 0, that part of the build only reaches the score through the
/// millionth that breaks ties, and the search leaves it out: it's exact up
/// to that tie-break.
std::optional<EnchantResult> BestEnchantments(const Model& model, Scorer& scorer,
                                              const std::array<std::int32_t, kSlots>& items,
                                              const EnchantSettings& settings, double incumbent,
                                              SearchCounts* counts = nullptr);

}  // namespace rotmg::search
