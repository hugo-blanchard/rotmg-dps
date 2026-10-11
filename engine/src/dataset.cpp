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

Class ReadClass(const json& j) {
  Class c;
  j.at("type").get_to(c.type);
  j.at("id").get_to(c.id);
  j.at("weaponSlotType").get_to(c.weapon_slot_type);
  c.attack = ReadStat(j.at("attack"));
  c.dexterity = ReadStat(j.at("dexterity"));
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
  if (auto it = j.find("statBonuses"); it != j.end()) {
    for (const json& b : *it) {
      w.stat_bonuses.push_back({b.at("stat").get<std::string>(), b.at("amount").get<int>()});
    }
  }
  for (const json& p : j.at("projectiles")) {
    w.projectiles.push_back(ReadProjectile(p));
  }
  for (const json& s : j.at("subattacks")) {
    w.subattacks.push_back(ReadSubattack(s));
  }
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
    return d;
  } catch (const json::exception& e) {
    throw DatasetError(std::string("bad dataset: ") + e.what());
  }
}

const Weapon* FindWeapon(const Dataset& dataset, std::string_view name) {
  auto it = std::ranges::find(dataset.weapons, name, &Weapon::name);
  return it == dataset.weapons.end() ? nullptr : &*it;
}

}  // namespace rotmg
