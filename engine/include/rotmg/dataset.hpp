#pragma once

#include <stdexcept>
#include <string_view>

#include "rotmg/model.hpp"

namespace rotmg {

class DatasetError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Throws DatasetError if the JSON is malformed, a required field is missing,
/// or the schema version isn't the one this engine reads.
Dataset ParseDataset(std::string_view json);

/// Looks a weapon up by display name. Null if there's no such weapon.
const Weapon* FindWeapon(const Dataset& dataset, std::string_view name);

/// Looks an ability up by display name. Null if there's no such ability.
const Ability* FindAbility(const Dataset& dataset, std::string_view name);

/// Looks an armor up by display name. Null if there's none.
const Gear* FindArmor(const Dataset& dataset, std::string_view name);

/// Looks a ring up by display name. Null if there's none.
const Gear* FindRing(const Dataset& dataset, std::string_view name);

/// Looks an enchantment up by id. Null if there's none.
const Enchantment* FindEnchantment(const Dataset& dataset, std::string_view id);

}  // namespace rotmg
