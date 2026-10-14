#pragma once

// A request as the engine reads it: what it asks for, then with every item
// and enchantment looked up. Shared by the API and the optimizer.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "rotmg/build.hpp"
#include "rotmg/damage.hpp"
#include "rotmg/dataset.hpp"
#include "rotmg/enchant.hpp"
#include "rotmg/evaluate.hpp"
#include "rotmg/model.hpp"
#include "rotmg/sets.hpp"
#include "rotmg/stats.hpp"

namespace rotmg {

struct Request {
  const Weapon* weapon = nullptr;
  const Ability* ability = nullptr;
  bool unknown_ability = false;
  Character character;
  Target target;
  Assumptions assumptions;
  std::vector<WornSet> sets;
  // The weapon with its enchantments, when it has any; `weapon` points here.
  std::shared_ptr<const Weapon> enchanted;
  // The ability with its enchantments, when they change it; `ability` points here.
  std::shared_ptr<const Ability> enchanted_ability;
  std::vector<StatBonus> enchantment_stats;
  // Assumptions the request's gear brings, for the build's list.
  std::vector<std::string> notes;
  const Gear* armor = nullptr;
  const Gear* ring = nullptr;
  // What the armor and ring do that isn't modelled.
  std::vector<std::string> gear_unsupported;
  // Stats the items' relative on-equip effects add.
  std::vector<StatBonus> item_stats;
  // How much the weapon's and ability's DPS count, which decides the
  // rhythm the ability is cast at.
  DpsWeights weights;
};

// What a request asks for, as read from its JSON, before anything is looked
// up. The optimizer fills one in for each build it scores, which costs far
// less than writing and reading JSON.
struct RequestSpec {
  std::string weapon;
  std::optional<std::string> ability;
  std::optional<std::string> armor;
  std::optional<std::string> ring;
  std::string class_id;
  // The base stats and status; gear and enchantments add to these.
  Character character;
  Target target;
  Assumptions assumptions;
  std::vector<int> wearing;
  bool artifacts = true;
  // Enchantment ids on the weapon, and on the other gear by "ability",
  // "armor" and "ring"; missing ones aren't asked about.
  std::optional<std::vector<std::string>> enchantments;
  std::optional<std::map<std::string, std::vector<std::string>, std::less<>>> gear_enchantments;
  DpsWeights weights;
};

// What a request's items and enchantments decide, whatever its base stats.
struct Prepared {
  // Its character's stats are worked out by Finish.
  Request request;
  // What gear and sets add, without the base stats; the rest of FinalStats'
  // base.
  StatBase stats;
  // What every item's enchantments add.
  EnchantStats enchantments;
  const Class* cls = nullptr;
};

// Throws on malformed JSON.
RequestSpec ReadSpec(const nlohmann::json& req);

// Everything in the spec but its base stats, looked up. Throws on an
// unknown armor, ring or enchantment; an unknown weapon leaves `weapon` null.
Prepared Prepare(const RequestSpec& spec, const Dataset& dataset);

// FinalStats' base for the prepared request on a character with `base`
// stats.
StatBase StatBaseOf(const Prepared& prepared, const Character& base);

// The prepared request on a character with `base` stats, through
// FinalStats.
Request Finish(const Prepared& prepared, const Character& base);

// The request a spec describes, with everything looked up. Throws on an
// unknown armor, ring or enchantment; an unknown weapon leaves `weapon` null.
Request Assemble(const RequestSpec& spec, const Dataset& dataset);

Request ParseRequest(const nlohmann::json& req, const Dataset& dataset);

}  // namespace rotmg
