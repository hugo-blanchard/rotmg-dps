#include "rotmg/sets.hpp"

#include <algorithm>
#include <set>

namespace rotmg {

std::vector<WornSet> WornSets(const Dataset& dataset, const std::vector<int>& worn) {
  std::vector<WornSet> out;
  for (const EquipmentSet& s : dataset.sets) {
    std::set<int> slots;
    for (const EquipmentSet::Piece& p : s.pieces) {
      if (std::ranges::find(worn, p.item_type) != worn.end()) {
        slots.insert(p.slot);
      }
    }
    if (slots.empty()) {
      continue;
    }
    WornSet w{&s, static_cast<int>(slots.size()), {}};
    for (const EquipmentSet::Bonus& b : s.bonuses) {
      if (b.pieces <= w.pieces) {
        w.stats.insert(w.stats.end(), b.stats.begin(), b.stats.end());
      }
    }
    out.push_back(std::move(w));
  }
  return out;
}

}  // namespace rotmg
