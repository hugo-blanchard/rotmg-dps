#include "model.hpp"

#include <utility>

namespace rotmg::search {

namespace {

StatEffect Compile(const Enchantment& e) {
  EnchantStats stats;
  bool stats_only = true;
  for (const Mutator& m : e.mutators) {
    stats_only = AddStatMutator(m, stats) && stats_only;
  }
  return {stats.tenths, std::move(stats.conversions), stats_only};
}

}  // namespace

Model::Model(const Dataset& dataset, RequestSpec base) : dataset_(&dataset), base_(std::move(base)) {
  effects_.reserve(dataset.enchantments.size());
  for (const Enchantment& e : dataset.enchantments) {
    effects_.push_back(Compile(e));
  }
}

const std::string& Model::ItemName(std::size_t slot, std::int32_t item) const {
  const auto i = static_cast<std::size_t>(item);
  switch (slot) {
    case kWeapon:
      return dataset_->weapons[i].name;
    case kAbility:
      return dataset_->abilities[i].name;
    case kArmor:
      return dataset_->armors[i].name;
    default:
      return dataset_->rings[i].name;
  }
}

}  // namespace rotmg::search
