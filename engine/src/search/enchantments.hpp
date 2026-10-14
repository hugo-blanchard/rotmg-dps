#pragma once

// The best enchantments for a fixed set of items, by a search that proves
// its answer rather than hoping to stumble on it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

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
/// Exact as long as what AssumptionsFor lists holds: more of a stat the
/// score reads never lowers it, no other stat changes it, the enchantments
/// it ignores can't change it, and each one it drops for a stronger one
/// never does better. The property tests check these on the fixture; an
/// audit checks them on real data. With a weight at 0, that part of the
/// build only reaches the score through the millionth that breaks ties,
/// and the search leaves it out: it's exact up to that tie-break.
/// What the search takes for granted about a set of items, so it can be
/// checked: the stats the score reads, the enchantments it never puts on an
/// item because they can't change the score, and the ones it drops for a
/// stronger one that fits wherever they do (weaker, stronger).
struct EnchantAssumptions {
  std::array<bool, kStats> read{};
  std::array<std::vector<std::int32_t>, kSlots> ignored;
  std::array<std::vector<std::pair<std::int32_t, std::int32_t>>, kSlots> replaced;
};

std::optional<EnchantResult> BestEnchantments(const Model& model, Scorer& scorer,
                                              const std::array<std::int32_t, kSlots>& items,
                                              const EnchantSettings& settings, double incumbent,
                                              SearchCounts* counts = nullptr);

EnchantAssumptions AssumptionsFor(const Model& model, Scorer& scorer, const std::array<std::int32_t, kSlots>& items,
                                  const EnchantSettings& settings);

}  // namespace rotmg::search
