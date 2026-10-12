#include "rotmg/sets.hpp"

#include <algorithm>
#include <set>

namespace rotmg {

std::vector<WornSet> WornSets(const Dataset& dataset, const std::vector<int>& worn) {
  const auto wearing = [&](int type) { return std::ranges::find(worn, type) != worn.end(); };
  std::vector<WornSet> out;
  for (const EquipmentSet& s : dataset.sets) {
    // Every Alien core's set lists the same Alien weapons and armors, but
    // only the core worn counts them (RealmEye, Alien Core: Power).
    if (!s.key_pieces.empty() && std::ranges::none_of(s.key_pieces, wearing)) {
      continue;
    }
    std::set<int> slots;
    for (const EquipmentSet::Piece& p : s.pieces) {
      if (wearing(p.item_type)) {
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
