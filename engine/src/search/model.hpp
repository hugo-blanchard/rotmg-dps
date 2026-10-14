#pragma once

// What the exact optimizer needs from a request, looked up once: items and
// enchantments as indices into the dataset, and each enchantment's stat
// effects compiled so that summing them is integer arithmetic.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../request.hpp"
#include "rotmg/dataset.hpp"
#include "rotmg/model.hpp"
#include "rotmg/stats.hpp"

namespace rotmg::search {

/// Weapon, ability, armor, ring.
inline constexpr std::size_t kSlots = 4;
enum Slot : std::size_t { kWeapon = 0, kAbility = 1, kArmor = 2, kRing = 3 };

/// What an enchantment does to stats.
struct StatEffect {
  Tenths tenths{};
  std::vector<Conversion> conversions;
  /// Nothing but flat stats and conversions: it changes no item's behaviour,
  /// only the character's stats.
  bool stats_only = true;
};

/// Items and enchantments by index into the dataset's lists: weapons,
/// abilities, armors, rings, enchantments. -1 is an empty slot.
struct Build {
  std::array<std::int32_t, kSlots> items{-1, -1, -1, -1};
  std::array<std::vector<std::int32_t>, kSlots> enchants;
};

class Model {
 public:
  Model(const Dataset& dataset, RequestSpec base);

  const Dataset& dataset() const { return *dataset_; }
  /// The request the builds are scored in: base stats, class, target,
  /// assumptions. Its items and enchantments are ignored.
  const RequestSpec& base() const { return base_; }
  const StatEffect& effect(std::int32_t enchantment) const {
    return effects_[static_cast<std::size_t>(enchantment)];
  }
  const Enchantment& enchantment(std::int32_t index) const {
    return dataset_->enchantments[static_cast<std::size_t>(index)];
  }
  /// The item's name, which is how requests name items.
  const std::string& ItemName(std::size_t slot, std::int32_t item) const;

 private:
  const Dataset* dataset_;
  RequestSpec base_;
  std::vector<StatEffect> effects_;
};

}  // namespace rotmg::search
