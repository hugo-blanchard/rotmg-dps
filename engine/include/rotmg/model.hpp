#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The dataset as the engine sees it. Field meanings and units are documented
// in the README's dataset format section; values are the client's own.
namespace rotmg {

struct StatRange {
  int base = 0;
  int max = 0;
};

struct Class {
  int type = 0;
  std::string id;
  int weapon_slot_type = 0;
  StatRange attack;
  StatRange dexterity;
};

struct StatBonus {
  std::string stat;
  int amount = 0;
};

struct ConditionEffect {
  std::string effect;
  double duration = 0;
};

struct Projectile {
  int id = 0;
  std::string object_id;
  double speed = 0;
  int min_damage = 0;
  int max_damage = 0;
  double lifetime_ms = 0;
  bool armor_piercing = false;
  bool multi_hit = false;
  bool boomerang = false;
  bool wavy = false;
  bool parametric = false;
  std::optional<double> size;
  std::optional<double> amplitude;
  std::optional<double> frequency;
  std::optional<double> magnitude;
  std::optional<double> acceleration;
  std::optional<double> acceleration_delay;
  std::optional<double> speed_clamp;
  std::vector<ConditionEffect> condition_effects;
};

struct Subattack {
  int projectile_id = 0;
  int num_projectiles = 1;
  double rate_of_fire = 1;
  std::optional<double> arc_gap;
  std::optional<double> default_angle;
  std::optional<std::array<double, 2>> pos_offset;
  std::optional<int> burst_count;
  std::optional<double> burst_delay;
  std::optional<double> burst_min_delay;
};

struct Weapon {
  int type = 0;
  std::string id;
  std::string name;
  int slot_type = 0;
  std::optional<int> tier;
  std::string texture_file;
  int texture_index = 0;
  std::vector<StatBonus> stat_bonuses;
  std::vector<Projectile> projectiles;
  std::vector<Subattack> subattacks;
  std::vector<std::string> unsupported;

  /// Null if the weapon has no projectile with that id.
  const Projectile* FindProjectile(int projectile_id) const;
  /// Sum of the weapon's IncrementStat bonuses for a client stat name like "ATT".
  int StatBonusFor(std::string_view stat) const;
};

struct Dataset {
  int schema_version = 0;
  std::string build;
  std::vector<Class> classes;
  std::vector<Weapon> weapons;
};

}  // namespace rotmg
