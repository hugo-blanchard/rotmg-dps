#include "request.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "rotmg/ability.hpp"
#include "rotmg/hits.hpp"

namespace rotmg {

using nlohmann::json;

RequestSpec ReadSpec(const json& req) {
  RequestSpec spec;
  spec.weapon = req.at("weapon").get<std::string>();
  if (auto it = req.find("ability"); it != req.end() && !it->is_null()) {
    spec.ability = it->get<std::string>();
  }
  for (const auto& [key, into] : {std::pair{"armor", &spec.armor}, {"ring", &spec.ring}}) {
    if (auto it = req.find(key); it != req.end() && !it->is_null()) {
      *into = it->get<std::string>();
    }
  }
  if (auto it = req.find("class"); it != req.end() && it->is_string()) {
    spec.class_id = it->get<std::string>();
  }
  Character& c = spec.character;
  c.attack = req.value("attack", 0);
  c.dexterity = req.value("dexterity", 0);
  c.wisdom = req.value("wisdom", 0);
  c.vitality = req.value("vitality", 0);
  c.defense = req.value("defense", 0);
  c.speed = req.value("speed", 0);
  c.max_hit_points = req.value("maxHitPoints", 0);
  c.max_magic_points = req.value("maxMagicPoints", 0);
  if (auto it = req.find("status"); it != req.end()) {
    c.status.weak = it->value("weak", false);
    c.status.damaging = it->value("damaging", false);
    c.status.berserk = it->value("berserk", false);
    c.status.dazed = it->value("dazed", false);
  }
  if (auto it = req.find("target"); it != req.end()) {
    spec.target.defense = it->value("defense", 0);
    spec.target.armored = it->value("armored", false);
    spec.target.armor_broken = it->value("armorBroken", false);
    spec.target.exposed = it->value("exposed", false);
    spec.target.cursed = it->value("cursed", false);
  }
  const auto read_hits = [&req](const char* key, std::vector<std::optional<double>>& out) {
    if (auto it = req.find(key); it != req.end()) {
      for (const json& h : *it) {
        out.push_back(h.is_null() ? std::nullopt : std::optional<double>(h.get<double>()));
      }
    }
  };
  read_hits("hitsPerVolley", spec.assumptions.hits_per_volley);
  read_hits("curatedHitsPerVolley", spec.assumptions.curated_hits_per_volley);
  if (auto it = req.find("hitModel"); it != req.end()) {
    spec.assumptions.distance = it->value("distance", 4.0);
    spec.assumptions.target_radius = it->value("radius", kDefaultTargetRadius);
  }
  spec.wearing = req.value("wearing", std::vector<int>{});
  spec.artifacts = req.value("artifacts", true);
  if (auto it = req.find("weights"); it != req.end()) {
    spec.weights.weapon = std::max(0.0, it->value("weapon", 1.0));
    spec.weights.ability = std::max(0.0, it->value("ability", 1.0));
  }
  if (auto it = req.find("enchantments"); it != req.end()) {
    spec.enchantments = it->get<std::vector<std::string>>();
  }
  if (auto it = req.find("gearEnchantments"); it != req.end()) {
    spec.gear_enchantments.emplace();
    for (const auto& [key, ids] : it->items()) {
      (*spec.gear_enchantments)[key] = ids.get<std::vector<std::string>>();
    }
  }
  return spec;
}

Prepared Prepare(const RequestSpec& spec, const Dataset& dataset) {
  Prepared out;
  Request& r = out.request;
  r.weapon = FindWeapon(dataset, spec.weapon);
  if (spec.ability) {
    r.ability = FindAbility(dataset, *spec.ability);
    r.unknown_ability = r.ability == nullptr;
  }
  r.target = spec.target;
  r.assumptions = spec.assumptions;
  r.weights = spec.weights;
  // The armor and ring add their stats like the weapon and ability do.
  std::vector<int> worn = spec.wearing;
  for (const auto& [key, list, into, name] : {std::tuple{"armor", &dataset.armors, &r.armor, &spec.armor},
                                              {"ring", &dataset.rings, &r.ring, &spec.ring}}) {
    if (*name) {
      *into = FindGear(*list, **name);
      if (*into == nullptr) {
        throw std::runtime_error(std::string("no ") + key + " named \"" + **name + "\"");
      }
      r.character = r.character.With((*into)->stat_bonuses);
      worn.push_back((*into)->type);
      for (const std::string& tag : (*into)->unsupported) {
        if (std::ranges::find(r.gear_unsupported, tag) == r.gear_unsupported.end()) {
          r.gear_unsupported.push_back(tag);
        }
      }
    }
  }
  // Set bonuses count the weapon and ability plus the other pieces worn,
  // and add to the character like gear does.
  if (r.weapon) {
    worn.push_back(r.weapon->type);
  }
  if (r.ability) {
    worn.push_back(r.ability->type);
  }
  r.sets = WornSets(dataset, worn);
  for (const WornSet& s : r.sets) {
    r.character = r.character.With(s.stats);
  }
  r.character.worn = worn;
  // Reads one item's enchantment ids, checking the item can have each, that
  // none clash and that they fit in its slots.
  const bool artifacts = spec.artifacts;
  const auto pick = [&](const std::vector<std::string>& ids, const std::string& item, const auto& obtained,
                        int slots) {
    std::vector<const Enchantment*> picked;
    for (const std::string& id : ids) {
      const auto e = std::ranges::find(dataset.enchantments, id, &Enchantment::id);
      if (e == dataset.enchantments.end()) {
        throw std::runtime_error("no enchantment named \"" + id + "\"");
      }
      const Obtained how = obtained(*e);
      if (how == Obtained::kNever && Legacy(*e)) {
        throw std::runtime_error(e->name + " is a legacy enchantment, which can't be obtained");
      }
      if (how == Obtained::kNever && slots == 0) {
        throw std::runtime_error(item + " can't be enchanted");
      }
      if (how == Obtained::kNever) {
        throw std::runtime_error(e->name + " doesn't go on " + item);
      }
      if (how == Obtained::kArtifact && !artifacts) {
        throw std::runtime_error(e->name + " only goes on " + item + " with an artifact");
      }
      if (how == Obtained::kArtifact) {
        r.notes.push_back(e->name + " only goes on " + item + " with an artifact or an engraving");
      }
      for (const Enchantment* other : picked) {
        if (Conflict(*e, *other)) {
          throw std::runtime_error(e->name + " and " + other->name + " can't share an item");
        }
      }
      picked.push_back(&*e);
    }
    if (std::cmp_greater(picked.size(), slots)) {
      throw std::runtime_error(item + " holds at most " + std::to_string(slots) + " enchantments");
    }
    return picked;
  };
  const auto& on_weapon = spec.enchantments;
  const auto& on_gear = spec.gear_enchantments;
  if ((on_weapon || on_gear) && r.weapon != nullptr) {
    const std::vector<const Enchantment*> picked =
        !on_weapon
            ? std::vector<const Enchantment*>{}
            : pick(*on_weapon, r.weapon->name,
                   [&](const Enchantment& e) { return HowObtained(e, *r.weapon, dataset); },
                   EnchantmentSlots(r.weapon->enchanting, dataset));
    GearEnchantments gear;
    // A named item takes what its own labels and pool allow; otherwise any
    // item of the slot.
    const auto on_slot = [&](const char* key, const char* label, const auto* item) {
      const auto ids = on_gear->find(std::string_view(key));
      if (ids == on_gear->end()) {
        return;
      }
      if (item == nullptr) {
        gear.emplace_back(label, pick(ids->second, std::string("your ") + key,
                                      [&](const Enchantment& e) { return HowObtainedOnSlot(e, label); }, 4));
      } else {
        gear.emplace_back(label, pick(ids->second, item->name,
                                      [&](const Enchantment& e) { return HowObtained(e, *item, dataset); },
                                      EnchantmentSlots(item->enchanting, dataset)));
      }
    };
    if (on_gear) {
      on_slot("ring", "RING", r.ring);
      on_slot("armor", "ARMOR", r.armor);
      on_slot("ability", "ABILITY", r.ability);
    }
    Enchanted en = Enchant(*r.weapon, picked, gear);
    if (en.mp_cost != 1 && r.ability != nullptr && r.ability->mp_cost) {
      Ability cheaper = *r.ability;
      cheaper.mp_cost = *cheaper.mp_cost * en.mp_cost;
      r.enchanted_ability = std::make_shared<const Ability>(std::move(cheaper));
      r.ability = r.enchanted_ability.get();
    }
    // Any stat the flat amounts move by a point or more, after rounding down.
    if (std::ranges::any_of(en.stats.tenths, [](std::int32_t t) { return t < 0 || t >= 10; })) {
      r.notes.emplace_back("fractional stat bonuses from enchantments add up and the total rounds down");
    }
    out.enchantments = std::move(en.stats);
    en.weapon.unsupported.insert(en.weapon.unsupported.end(), en.unsupported.begin(), en.unsupported.end());
    r.enchanted = std::make_shared<const Weapon>(std::move(en.weapon));
    r.weapon = r.enchanted.get();
  }
  // The items' own on-equip effects besides flat stats.
  double discount = 1;
  const auto equip = [&](const std::vector<Activate>& on_equip) {
    for (const Activate& a : on_equip) {
      if (AddOwnConversion(a, out.stats.own)) {
        continue;
      }
      if (a.type == "AbilityUseDiscount") {
        // RealmEye, The Twilight Gemstone: its cut stacks with other gear's.
        discount *= a.Number("multiplier").value_or(1);
      } else if (const std::string tag = "ActivateOnEquip " + a.type;
                 std::ranges::find(r.gear_unsupported, tag) == r.gear_unsupported.end()) {
        r.gear_unsupported.push_back(tag);
      }
    }
  };
  if (r.weapon != nullptr) {
    equip(r.weapon->on_equip);
  }
  if (r.ability != nullptr) {
    equip(r.ability->on_equip);
  }
  for (const Gear* g : {r.armor, r.ring}) {
    if (g != nullptr) {
      equip(g->on_equip);
    }
  }
  if (discount != 1 && r.ability != nullptr && r.ability->mp_cost) {
    Ability cheaper = *r.ability;
    cheaper.mp_cost = *cheaper.mp_cost * discount;
    r.enchanted_ability = std::make_shared<const Ability>(std::move(cheaper));
    r.ability = r.enchanted_ability.get();
  }
  const auto found = std::ranges::find(dataset.classes, spec.class_id, &Class::id);
  out.cls = found == dataset.classes.end() ? nullptr : &*found;
  out.stats.before = ToStats(r.character);
  if (r.weapon != nullptr) {
    out.stats.item_bonuses = ToStats(r.weapon->stat_bonuses);
  }
  if (r.ability != nullptr) {
    const Stats ability = ToStats(r.ability->stat_bonuses);
    for (std::size_t s = 0; s < kStats; ++s) {
      out.stats.item_bonuses[s] += ability[s];
    }
  }
  if (out.cls != nullptr) {
    out.stats.has_class = true;
    out.stats.class_max = {out.cls->attack.max,   out.cls->dexterity.max, out.cls->speed.max,
                           out.cls->wisdom.max,   out.cls->vitality.max,  out.cls->defense.max,
                           out.cls->max_hit_points.max, out.cls->max_magic_points.max};
  }
  // The other gear's on-shoot effects go off as the weapon fires, like its
  // own, and some create their projectiles.
  const auto join = [&](const std::vector<Activate>& on_shoot, const std::map<int, Projectile>& created,
                        const Ability* own) {
    if (on_shoot.empty() || r.weapon == nullptr) {
      return;
    }
    Weapon w = *r.weapon;
    for (Activate a : on_shoot) {
      // An ability's shots without a `type` are its own projectile, which
      // the weapon knows by the ability's type.
      if (own != nullptr && a.Text("type").empty() && !own->projectiles.empty()) {
        a.params["type"] = std::to_string(own->type);
        w.created_projectiles.emplace(own->type, own->projectiles[0]);
      }
      w.on_shoot.push_back(std::move(a));
    }
    w.created_projectiles.insert(created.begin(), created.end());
    r.enchanted = std::make_shared<const Weapon>(std::move(w));
    r.weapon = r.enchanted.get();
  };
  if (r.ability != nullptr) {
    join(r.ability->on_shoot, r.ability->created_projectiles, r.ability);
  }
  for (const Gear* g : {r.armor, r.ring}) {
    if (g != nullptr) {
      join(g->on_shoot, g->created_projectiles, nullptr);
    }
  }
  // What the other gear does when the ability is used goes with the
  // ability's own on-use effects.
  if (r.ability != nullptr) {
    Ability with = *r.ability;
    const auto add = [&](const std::vector<Activate>& on_ability, const std::map<int, Projectile>& created) {
      with.on_ability.insert(with.on_ability.end(), on_ability.begin(), on_ability.end());
      with.created_projectiles.insert(created.begin(), created.end());
    };
    if (const Weapon* w = FindWeapon(dataset, spec.weapon)) {
      add(w->on_ability, w->created_projectiles);
    }
    for (const Gear* g : {r.armor, r.ring}) {
      if (g != nullptr) {
        add(g->on_ability, g->created_projectiles);
      }
    }
    if (with.on_ability.size() != r.ability->on_ability.size()) {
      r.enchanted_ability = std::make_shared<const Ability>(std::move(with));
      r.ability = r.enchanted_ability.get();
    }
  }
  return out;
}

StatBase StatBaseOf(const Prepared& prepared, const Character& base) {
  StatBase out = prepared.stats;
  const Stats more = ToStats(base);
  for (std::size_t s = 0; s < kStats; ++s) {
    out.before[s] += more[s];
  }
  return out;
}

Request Finish(const Prepared& prepared, const Character& base) {
  Request r = prepared.request;
  const StatBase stats = StatBaseOf(prepared, base);
  const EnchantStats& enchantments = prepared.enchantments;
  StatParts parts;
  r.character = WithStats(r.character, FinalStats(stats, enchantments.tenths, enchantments.conversions, &parts));
  r.character.status = base.status;
  Stats from_enchantments{};
  for (std::size_t s = 0; s < kStats; ++s) {
    from_enchantments[s] = parts.flat[s] + parts.converted[s];
  }
  r.enchantment_stats = BonusesOf(from_enchantments);
  r.item_stats = BonusesOf(parts.own);
  const bool relative = !enchantments.conversions.empty();
  if (prepared.cls == nullptr &&
      (relative || std::ranges::any_of(stats.own, [](const Conversion& c) { return !c.of_total; }))) {
    r.gear_unsupported.emplace_back("relative stat bonuses, which need the class");
  }
  if (relative && prepared.cls != nullptr) {
    r.notes.emplace_back(
        "relative enchantments count stats above the class's max as bonus stats, as on a maxed character, and "
        "round down");
  }
  if (!r.item_stats.empty()) {
    r.notes.emplace_back("an item's share of one stat added to another rounds down, and counts with relative "
                         "enchantments in one step");
  }
  return r;
}

Request Assemble(const RequestSpec& spec, const Dataset& dataset) {
  return Finish(Prepare(spec, dataset), spec.character);
}

// Reads the parts of a request Evaluate and StartSimulation share. Throws on
// malformed JSON; an unknown weapon leaves `weapon` null.
Request ParseRequest(const json& req, const Dataset& dataset) { return Assemble(ReadSpec(req), dataset); }

}  // namespace rotmg
