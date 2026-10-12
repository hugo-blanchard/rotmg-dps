#include "rotmg/dataset.hpp"

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

#include "rotmg/schema.hpp"

namespace rotmg {
namespace {

using nlohmann::json;

template <typename T>
void ReadOptional(const json& j, const char* key, std::optional<T>& out) {
  if (auto it = j.find(key); it != j.end()) {
    out = it->get<T>();
  }
}

template <typename T>
void ReadDefault(const json& j, const char* key, T& out) {
  if (auto it = j.find(key); it != j.end()) {
    it->get_to(out);
  }
}

StatRange ReadStat(const json& j) {
  return {j.at("base").get<int>(), j.at("max").get<int>()};
}

void ReadOptionalStat(const json& j, const char* key, StatRange& out) {
  if (auto it = j.find(key); it != j.end()) {
    out = ReadStat(*it);
  }
}

Class ReadClass(const json& j) {
  Class c;
  j.at("type").get_to(c.type);
  j.at("id").get_to(c.id);
  j.at("weaponSlotType").get_to(c.weapon_slot_type);
  ReadDefault(j, "abilitySlotType", c.ability_slot_type);
  c.attack = ReadStat(j.at("attack"));
  c.dexterity = ReadStat(j.at("dexterity"));
  ReadOptionalStat(j, "maxHitPoints", c.max_hit_points);
  ReadOptionalStat(j, "maxMagicPoints", c.max_magic_points);
  ReadOptionalStat(j, "defense", c.defense);
  ReadOptionalStat(j, "speed", c.speed);
  ReadOptionalStat(j, "vitality", c.vitality);
  ReadOptionalStat(j, "wisdom", c.wisdom);
  return c;
}

Projectile ReadProjectile(const json& j) {
  Projectile p;
  j.at("id").get_to(p.id);
  j.at("objectId").get_to(p.object_id);
  j.at("speed").get_to(p.speed);
  j.at("minDamage").get_to(p.min_damage);
  j.at("maxDamage").get_to(p.max_damage);
  j.at("lifetimeMs").get_to(p.lifetime_ms);
  ReadDefault(j, "armorPiercing", p.armor_piercing);
  ReadDefault(j, "multiHit", p.multi_hit);
  ReadDefault(j, "boomerang", p.boomerang);
  ReadDefault(j, "wavy", p.wavy);
  ReadDefault(j, "parametric", p.parametric);
  ReadOptional(j, "size", p.size);
  ReadOptional(j, "amplitude", p.amplitude);
  ReadOptional(j, "frequency", p.frequency);
  ReadOptional(j, "magnitude", p.magnitude);
  ReadOptional(j, "acceleration", p.acceleration);
  ReadOptional(j, "accelerationDelay", p.acceleration_delay);
  ReadOptional(j, "speedClamp", p.speed_clamp);
  ReadOptional(j, "circleTurnDelay", p.circle_turn_delay);
  ReadOptional(j, "circleTurnAngle", p.circle_turn_angle);
  if (auto it = j.find("conditionEffects"); it != j.end()) {
    for (const json& c : *it) {
      p.condition_effects.push_back({c.at("effect").get<std::string>(), c.at("duration").get<double>()});
    }
  }
  return p;
}

Subattack ReadSubattack(const json& j) {
  Subattack s;
  j.at("projectileId").get_to(s.projectile_id);
  j.at("numProjectiles").get_to(s.num_projectiles);
  j.at("rateOfFire").get_to(s.rate_of_fire);
  ReadOptional(j, "arcGap", s.arc_gap);
  ReadOptional(j, "defaultAngle", s.default_angle);
  ReadOptional(j, "posOffset", s.pos_offset);
  ReadOptional(j, "burstCount", s.burst_count);
  ReadOptional(j, "burstDelay", s.burst_delay);
  ReadOptional(j, "burstMinDelay", s.burst_min_delay);
  return s;
}

std::vector<StatBonus> ReadStatBonuses(const json& j) {
  std::vector<StatBonus> out;
  if (auto it = j.find("statBonuses"); it != j.end()) {
    for (const json& b : *it) {
      out.push_back({b.at("stat").get<std::string>(), b.at("amount").get<int>()});
    }
  }
  return out;
}

std::vector<Activate> ReadActivates(const json& j, std::string_view key) {
  std::vector<Activate> out;
  auto it = j.find(key);
  if (it == j.end()) {
    return out;
  }
  for (const json& act : *it) {
    Activate a;
    act.at("type").get_to(a.type);
    if (auto params = act.find("params"); params != act.end()) {
      for (const auto& [name, value] : params->items()) {
        if (value.is_number()) {
          a.params.emplace(name, value.get<double>());
        } else if (value.is_boolean()) {
          a.params.emplace(name, value.get<bool>());
        } else if (value.is_string()) {
          a.params.emplace(name, value.get<std::string>());
        }
      }
    }
    out.push_back(std::move(a));
  }
  return out;
}

Activate::Value ReadValue(const json& v) {
  if (v.is_number()) {
    return v.get<double>();
  }
  if (v.is_boolean()) {
    return v.get<bool>();
  }
  return v.is_string() ? v.get<std::string>() : std::string();
}

Enchantment ReadEnchantment(const json& j) {
  Enchantment e;
  j.at("type").get_to(e.type);
  j.at("id").get_to(e.id);
  e.name = j.value("name", e.id);
  e.description = j.value("description", "");
  ReadDefault(j, "compatible", e.compatible);
  ReadDefault(j, "incompatible", e.incompatible);
  ReadDefault(j, "incompatibleItems", e.incompatible_items);
  ReadDefault(j, "labels", e.labels);
  ReadDefault(j, "incompatibleLabels", e.incompatible_labels);
  if (auto it = j.find("mutators"); it != j.end()) {
    for (const json& m : *it) {
      Mutator out;
      m.at("tag").get_to(out.tag);
      if (auto v = m.find("value"); v != m.end()) {
        if (v->is_number()) {
          out.number = v->get<double>();
        } else if (v->is_string()) {
          out.text = v->get<std::string>();
        }
      }
      if (auto params = m.find("params"); params != m.end()) {
        for (const auto& [name, value] : params->items()) {
          out.attributes.params.emplace(name, ReadValue(value));
        }
      }
      e.mutators.push_back(std::move(out));
    }
  }
  return e;
}

std::map<int, Projectile> ReadCreatedProjectiles(const json& j) {
  std::map<int, Projectile> out;
  if (auto it = j.find("createdProjectiles"); it != j.end()) {
    for (const json& c : *it) {
      out.emplace(c.at("type").get<int>(), ReadProjectile(c.at("projectile")));
    }
  }
  return out;
}

Ability ReadAbility(const json& j) {
  Ability a;
  j.at("type").get_to(a.type);
  j.at("id").get_to(a.id);
  j.at("name").get_to(a.name);
  j.at("slotType").get_to(a.slot_type);
  ReadOptional(j, "tier", a.tier);
  ReadOptional(j, "mpCost", a.mp_cost);
  ReadOptional(j, "cooldown", a.cooldown);
  a.stat_bonuses = ReadStatBonuses(j);
  if (auto it = j.find("projectiles"); it != j.end()) {
    for (const json& p : *it) {
      a.projectiles.push_back(ReadProjectile(p));
    }
  }
  if (auto it = j.find("subattacks"); it != j.end()) {
    for (const json& s : *it) {
      a.subattacks.push_back(ReadSubattack(s));
    }
  }
  a.activates = ReadActivates(j, "activates");
  a.created_projectiles = ReadCreatedProjectiles(j);
  ReadDefault(j, "unsupported", a.unsupported);
  return a;
}

EquipmentSet ReadSet(const json& j) {
  EquipmentSet s;
  j.at("type").get_to(s.type);
  j.at("name").get_to(s.name);
  for (const json& p : j.at("pieces")) {
    s.pieces.push_back({p.at("slot").get<int>(), p.at("itemType").get<int>(), p.value("name", "")});
  }
  if (auto it = j.find("bonuses"); it != j.end()) {
    for (const json& b : *it) {
      s.bonuses.push_back({b.at("pieces").get<int>(), ReadStatBonuses({{"statBonuses", b.at("stats")}})});
    }
  }
  ReadDefault(j, "unsupported", s.unsupported);
  return s;
}

Weapon ReadWeapon(const json& j) {
  Weapon w;
  j.at("type").get_to(w.type);
  j.at("id").get_to(w.id);
  j.at("name").get_to(w.name);
  j.at("slotType").get_to(w.slot_type);
  ReadOptional(j, "tier", w.tier);
  const json& texture = j.at("texture");
  texture.at("file").get_to(w.texture_file);
  texture.at("index").get_to(w.texture_index);
  w.stat_bonuses = ReadStatBonuses(j);
  for (const json& p : j.at("projectiles")) {
    w.projectiles.push_back(ReadProjectile(p));
  }
  for (const json& s : j.at("subattacks")) {
    w.subattacks.push_back(ReadSubattack(s));
  }
  w.on_shoot = ReadActivates(j, "onShoot");
  w.created_projectiles = ReadCreatedProjectiles(j);
  ReadDefault(j, "labels", w.labels);
  ReadDefault(j, "unsupported", w.unsupported);
  return w;
}

}  // namespace

const Projectile* Weapon::FindProjectile(int projectile_id) const {
  auto it = std::ranges::find(projectiles, projectile_id, &Projectile::id);
  return it == projectiles.end() ? nullptr : &*it;
}

int Weapon::StatBonusFor(std::string_view stat) const {
  int total = 0;
  for (const StatBonus& b : stat_bonuses) {
    if (b.stat == stat) {
      total += b.amount;
    }
  }
  return total;
}

Dataset ParseDataset(std::string_view text) {
  try {
    const json j = json::parse(text);
    Dataset d;
    j.at("schemaVersion").get_to(d.schema_version);
    if (!SupportsSchema(d.schema_version)) {
      throw DatasetError("dataset schema version " + std::to_string(d.schema_version) +
                         ", this engine reads version " + std::to_string(kSchemaVersion));
    }
    j.at("build").get_to(d.build);
    for (const json& c : j.at("classes")) {
      d.classes.push_back(ReadClass(c));
    }
    for (const json& w : j.at("weapons")) {
      d.weapons.push_back(ReadWeapon(w));
    }
    if (auto it = j.find("abilities"); it != j.end()) {
      for (const json& a : *it) {
        d.abilities.push_back(ReadAbility(a));
      }
    }
    if (auto it = j.find("sets"); it != j.end()) {
      for (const json& s : *it) {
        d.sets.push_back(ReadSet(s));
      }
    }
    if (auto it = j.find("enchantments"); it != j.end()) {
      for (const json& e : *it) {
        d.enchantments.push_back(ReadEnchantment(e));
      }
    }
    return d;
  } catch (const json::exception& e) {
    throw DatasetError(std::string("bad dataset: ") + e.what());
  }
}

const Weapon* FindWeapon(const Dataset& dataset, std::string_view name) {
  auto it = std::ranges::find(dataset.weapons, name, &Weapon::name);
  return it == dataset.weapons.end() ? nullptr : &*it;
}

const Ability* FindAbility(const Dataset& dataset, std::string_view name) {
  auto it = std::ranges::find(dataset.abilities, name, &Ability::name);
  return it == dataset.abilities.end() ? nullptr : &*it;
}

std::optional<double> Activate::Number(std::string_view key) const {
  auto it = params.find(key);
  if (it == params.end()) {
    return std::nullopt;
  }
  if (const double* v = std::get_if<double>(&it->second)) {
    return *v;
  }
  return std::nullopt;
}

std::string Activate::Text(std::string_view key) const {
  auto it = params.find(key);
  if (it == params.end()) {
    return "";
  }
  if (const std::string* v = std::get_if<std::string>(&it->second)) {
    return *v;
  }
  return "";
}

bool Activate::Flag(std::string_view key) const {
  auto it = params.find(key);
  if (it == params.end()) {
    return false;
  }
  const bool* v = std::get_if<bool>(&it->second);
  return v != nullptr && *v;
}

Weapon Ability::AsWeapon() const {
  Weapon w;
  w.type = type;
  w.id = id;
  w.name = name;
  w.projectiles = projectiles;
  w.subattacks = subattacks;
  return w;
}

}  // namespace rotmg
