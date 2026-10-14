#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "rotmg/optimize.hpp"

namespace rotmg::search {

int StatIndex(std::string_view client_name) {
  const auto it = std::ranges::find(kOptimizerStats, client_name);
  return it == kOptimizerStats.end() ? -1 : static_cast<int>(it - kOptimizerStats.begin());
}

namespace {

int MaxOf(const Class& c, std::size_t stat) {
  const std::array<const StatRange*, kStats> ranges = {&c.attack,  &c.dexterity, &c.speed,          &c.wisdom,
                                                       &c.vitality, &c.defense,  &c.max_hit_points, &c.max_magic_points};
  return ranges[stat]->max;
}

StatEffect Compile(const Enchantment& e) {
  StatEffect out;
  for (const Mutator& m : e.mutators) {
    const int stat = StatIndex(m.attributes.Text("stat"));
    const double amount = m.attributes.Number("amount").value_or(0);
    if (m.tag == "ActivateOnEquip" && m.text == "IncrementStat") {
      const double tenths = amount * 10;
      if (std::abs(tenths - std::round(tenths)) > 1e-6) {
        throw std::runtime_error("stat amount " + std::to_string(amount) + " in enchantment \"" + e.id +
                                 "\" isn't a whole number of tenths");
      }
      if (stat >= 0) {
        out.tenths[static_cast<std::size_t>(stat)] += static_cast<std::int32_t>(std::lround(tenths));
      }
    } else if (m.tag == "ActivateOnEquip" && m.text == "BonusStatRelative") {
      const int from = StatIndex(m.attributes.Text("statRelativeTo"));
      if (stat >= 0 && from >= 0) {
        out.conversions.push_back(
            {static_cast<std::uint8_t>(from), static_cast<std::uint8_t>(stat), amount, false});
      }
    } else {
      out.stats_only = false;
    }
  }
  return out;
}

}  // namespace

Model::Model(const Dataset& dataset, RequestSpec base) : dataset_(&dataset), base_(std::move(base)) {
  if (const auto c = std::ranges::find(dataset.classes, base_.class_id, &Class::id); c != dataset.classes.end()) {
    for (std::size_t s = 0; s < kStats; ++s) {
      class_max_[s] = MaxOf(*c, s);
    }
  }
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
