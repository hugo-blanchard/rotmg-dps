#include "rotmg/dataset.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "rotmg/schema.hpp"

namespace rotmg {

// Effect parameters by key, like a.Number(kCooldown).
using enum ParamKey;
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
  ReadDefault(j, "armorSlotType", c.armor_slot_type);
  ReadDefault(j, "ringSlotType", c.ring_slot_type);
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
      // Flat stats are summed in tenths, exactly.
      if (out.tag == "ActivateOnEquip" && out.text == "IncrementStat") {
        const double amount = out.attributes.Number(kAmount).value_or(0);
        if (std::abs(amount * 10 - std::round(amount * 10)) > 1e-6) {
          throw DatasetError("stat amount " + std::to_string(amount) + " in enchantment \"" + e.id +
                             "\" isn't a whole number of tenths");
        }
      }
      e.mutators.push_back(std::move(out));
    }
  }
  if (auto it = j.find("containers"); it != j.end()) {
    for (const json& c : *it) {
      Enchantment::Container container;
      for (const json& p : c.at("projectiles")) {
        container.projectiles.push_back(ReadProjectile(p));
      }
      for (const json& s : c.at("subattacks")) {
        container.subattacks.push_back(ReadSubattack(s));
      }
      ReadDefault(c, "unsupported", container.unsupported);
      e.containers.emplace(c.at("id").get<std::string>(), std::move(container));
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

Enchanting ReadEnchanting(const json& j) {
  return {j.value("enchantmentSlots", 0), j.value("enchantmentPool", "")};
}

Gear ReadGear(const json& j) {
  Gear g;
  j.at("type").get_to(g.type);
  j.at("id").get_to(g.id);
  j.at("name").get_to(g.name);
  j.at("slotType").get_to(g.slot_type);
  ReadOptional(j, "tier", g.tier);
  g.stat_bonuses = ReadStatBonuses(j);
  ReadDefault(j, "labels", g.labels);
  g.on_shoot = ReadActivates(j, "onShoot");
  g.on_ability = ReadActivates(j, "onAbility");
  g.on_equip = ReadActivates(j, "onEquip");
  g.created_projectiles = ReadCreatedProjectiles(j);
  g.enchanting = ReadEnchanting(j);
  ReadDefault(j, "reskinOf", g.reskin_of);
  ReadDefault(j, "variantOf", g.variant_of);
  ReadDefault(j, "unobtainable", g.unobtainable);
  ReadDefault(j, "unsupported", g.unsupported);
  return g;
}

AbilityMode ReadAbilityMode(const json& j) {
  AbilityMode m;
  ReadDefault(j, "name", m.name);
  ReadOptional(j, "attackIndex", m.attack_index);
  ReadOptional(j, "mpCost", m.mp_cost);
  ReadOptional(j, "cooldown", m.cooldown);
  ReadOptional(j, "mpCostPerSecond", m.mp_cost_per_second);
  ReadOptional(j, "mpEndCost", m.mp_end_cost);
  ReadOptional(j, "abilityTime", m.ability_time);
  ReadOptional(j, "switchTime", m.switch_time);
  ReadDefault(j, "multiPhase", m.multi_phase);
  m.activates = ReadActivates(j, "activates");
  m.on_shoot = ReadActivates(j, "onShoot");
  m.on_ability = ReadActivates(j, "onAbility");
  m.on_switch = ReadActivates(j, "onSwitch");
  m.on_enemy_hit = ReadActivates(j, "onEnemyHit");
  m.on_heal = ReadActivates(j, "onHeal");
  return m;
}

// An ability with modes is used in its first until switching between them
// is modelled: its effects and costs, and with an attack index only that
// subattack.
void UseFirstMode(Ability& a) {
  if (a.modes.empty() || !a.activates.empty() || !a.on_shoot.empty()) {
    return;
  }
  const AbilityMode& m = a.modes.front();
  a.activates = m.activates;
  a.on_shoot = m.on_shoot;
  a.on_ability.insert(a.on_ability.end(), m.on_ability.begin(), m.on_ability.end());
  if (!a.mp_cost) a.mp_cost = m.mp_cost;
  if (!a.cooldown) a.cooldown = m.cooldown;
  a.mode_subattacks = a.subattacks;
  if (m.attack_index && *m.attack_index >= 0 && static_cast<std::size_t>(*m.attack_index) < a.subattacks.size()) {
    a.subattacks = {a.mode_subattacks[static_cast<std::size_t>(*m.attack_index)]};
  }
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
  a.on_shoot = ReadActivates(j, "onShoot");
  a.on_ability = ReadActivates(j, "onAbility");
  a.on_equip = ReadActivates(j, "onEquip");
  a.created_projectiles = ReadCreatedProjectiles(j);
  if (auto it = j.find("undead"); it != j.end()) {
    for (const json& u : *it) {
      Undead undead;
      for (const json& p : u.at("projectiles")) {
        undead.projectiles.push_back(ReadProjectile(p));
      }
      undead.shots = u.value("shots", 1);
      undead.arc_gap = u.value("arcGap", 0.0);
      undead.attacks_per_second = u.value("attacksPerSecond", 0.0);
      undead.bursts = u.value("bursts", 0);
      ReadDefault(u, "burstRadii", undead.burst_radii);
      undead.lifetime_s = u.value("lifetimeS", 0.0);
      a.undead.emplace(u.at("id").get<std::string>(), std::move(undead));
    }
  }
  if (auto it = j.find("modes"); it != j.end()) {
    for (const json& m : *it) {
      a.modes.push_back(ReadAbilityMode(m));
    }
  }
  if (auto it = j.find("meter"); it != j.end()) {
    a.meter = Meter{it->value("id", 0), it->at("maxValue").get<double>(), it->value("startValue", 0.0)};
  }
  ReadDefault(j, "labels", a.labels);
  a.enchanting = ReadEnchanting(j);
  ReadDefault(j, "reskinOf", a.reskin_of);
  ReadDefault(j, "variantOf", a.variant_of);
  ReadDefault(j, "unobtainable", a.unobtainable);
  ReadDefault(j, "unsupported", a.unsupported);
  UseFirstMode(a);
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
  ReadDefault(j, "keyPieces", s.key_pieces);
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
  w.on_enemy_hit = ReadActivates(j, "onEnemyHit");
  w.on_ability = ReadActivates(j, "onAbility");
  w.on_equip = ReadActivates(j, "onEquip");
  w.created_projectiles = ReadCreatedProjectiles(j);
  ReadDefault(j, "labels", w.labels);
  w.enchanting = ReadEnchanting(j);
  ReadDefault(j, "reskinOf", w.reskin_of);
  ReadDefault(j, "variantOf", w.variant_of);
  ReadDefault(j, "unobtainable", w.unobtainable);
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
    for (const auto& [key, into] : {std::pair{"armors", &d.armors}, {"rings", &d.rings}}) {
      if (auto it = j.find(key); it != j.end()) {
        for (const json& g : *it) {
          into->push_back(ReadGear(g));
        }
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
    if (auto it = j.find("enchantmentPools"); it != j.end()) {
      for (const json& p : *it) {
        d.enchantment_pools.emplace(p.at("name").get<std::string>(), p.at("enchantments").get<std::set<std::string>>());
      }
    }
    const auto index = [](const auto& list, NameIndex& into, auto key) {
      for (std::size_t i = 0; i < list.size(); ++i) {
        into.try_emplace(list[i].*key, i);
      }
    };
    index(d.weapons, d.index.weapons, &Weapon::name);
    index(d.abilities, d.index.abilities, &Ability::name);
    index(d.armors, d.index.armors, &Gear::name);
    index(d.rings, d.index.rings, &Gear::name);
    index(d.enchantments, d.index.enchantments, &Enchantment::id);
    return d;
  } catch (const json::exception& e) {
    throw DatasetError(std::string("bad dataset: ") + e.what());
  }
}

namespace {

// The first entry of `list` whose `key` is `name`, through the index when
// it covers the list. Names are unique in a dataset the pipeline made, so
// the index then has an entry for each.
template <typename T>
const T* Find(const std::vector<T>& list, const NameIndex& index, std::string_view name, std::string T::*key) {
  if (index.size() == list.size()) {
    const auto it = index.find(name);
    return it == index.end() ? nullptr : &list[it->second];
  }
  const auto it = std::ranges::find(list, name, key);
  return it == list.end() ? nullptr : &*it;
}

}  // namespace

const Weapon* FindWeapon(const Dataset& dataset, std::string_view name) {
  return Find(dataset.weapons, dataset.index.weapons, name, &Weapon::name);
}

const Ability* FindAbility(const Dataset& dataset, std::string_view name) {
  return Find(dataset.abilities, dataset.index.abilities, name, &Ability::name);
}

const Gear* FindArmor(const Dataset& dataset, std::string_view name) {
  return Find(dataset.armors, dataset.index.armors, name, &Gear::name);
}

const Gear* FindRing(const Dataset& dataset, std::string_view name) {
  return Find(dataset.rings, dataset.index.rings, name, &Gear::name);
}

const Enchantment* FindEnchantment(const Dataset& dataset, std::string_view id) {
  return Find(dataset.enchantments, dataset.index.enchantments, id, &Enchantment::id);
}

ParamKey ParamKeyOf(std::string_view name) {
  const auto it = std::ranges::lower_bound(kParamNames, name);
  return it != kParamNames.end() && *it == name ? static_cast<ParamKey>(it - kParamNames.begin()) : kOther;
}

namespace {

const Activate::Value* Lookup(const Params& params, std::string_view key) {
  const auto it = params.find(key);
  return it == params.end() ? nullptr : &it->second;
}

std::optional<double> NumberOf(const Activate::Value* v) {
  const double* d = v == nullptr ? nullptr : std::get_if<double>(v);
  return d == nullptr ? std::nullopt : std::optional<double>(*d);
}

std::string_view TextOf(const Activate::Value* v) {
  const std::string* s = v == nullptr ? nullptr : std::get_if<std::string>(v);
  return s == nullptr ? std::string_view() : std::string_view(*s);
}

bool FlagOf(const Activate::Value* v) {
  const bool* b = v == nullptr ? nullptr : std::get_if<bool>(v);
  return b != nullptr && *b;
}

}  // namespace

std::optional<double> Activate::Number(ParamKey key) const { return NumberOf(params.Find(key)); }
std::optional<double> Activate::Number(std::string_view key) const { return NumberOf(Lookup(params, key)); }
std::string_view Activate::Text(ParamKey key) const { return TextOf(params.Find(key)); }
std::string_view Activate::Text(std::string_view key) const { return TextOf(Lookup(params, key)); }
bool Activate::Flag(ParamKey key) const { return FlagOf(params.Find(key)); }
bool Activate::Flag(std::string_view key) const { return FlagOf(Lookup(params, key)); }

int Undead::Attacks() const {
  if (bursts > 0) {
    return bursts;
  }
  return static_cast<int>(std::floor(attacks_per_second * lifetime_s + 1e-9));
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
