#pragma once

#include <vector>

#include "rotmg/model.hpp"

namespace rotmg {

/// A set some of the worn items belong to, and what it adds.
struct WornSet {
  const EquipmentSet* set = nullptr;
  /// Slots holding a piece of the set.
  int pieces = 0;
  /// Every bonus for that many pieces, added up (RealmEye's set pages total
  /// the 2nd, 3rd and 4th piece bonuses).
  std::vector<StatBonus> stats;
};

/// The sets with at least one piece among `worn`, by item type.
std::vector<WornSet> WornSets(const Dataset& dataset, const std::vector<int>& worn);

}  // namespace rotmg
