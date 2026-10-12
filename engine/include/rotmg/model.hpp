#pragma once

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
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
  /// 0 in datasets from before abilities were added, as are the stats below
  /// that ATT and DEX.
  int ability_slot_type = 0;
  /// 0 in datasets from before armor and rings were added.
  int armor_slot_type = 0;
  int ring_slot_type = 0;
  StatRange max_hit_points;
  StatRange max_magic_points;
  StatRange attack;
  StatRange defense;
  StatRange speed;
  StatRange dexterity;
  StatRange vitality;
  StatRange wisdom;
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
  // CircleTurnDelay in ms and CircleTurnAngle in degrees.
  std::optional<double> circle_turn_delay;
  std::optional<double> circle_turn_angle;
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

/// One effect of using an item, with the client's parameters as they are:
/// numbers, booleans or strings.
struct Activate {
  using Value = std::variant<double, bool, std::string>;

  std::string type;
  std::map<std::string, Value, std::less<>> params;

  std::optional<double> Number(std::string_view key) const;
  std::string Text(std::string_view key) const;
  /// False when the parameter is missing or isn't a boolean.
  bool Flag(std::string_view key) const;
};

/// How an item takes enchantments: how many slots can roll and the pool
/// they roll from. An item that can't be enchanted has no slots.
struct Enchanting {
  int slots = 0;
  std::string pool;
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
  /// OnPlayerShootActivate effects, with the client's parameters.
  std::vector<Activate> on_shoot;
  /// Projectiles of other items, by type, that on-shoot effects create.
  std::map<int, Projectile> created_projectiles;
  /// The item's labels, which enchantments match against.
  std::vector<std::string> labels;
  Enchanting enchanting;
  /// The item this one plays exactly like, when it's a reskin of it.
  std::string reskin_of;
  /// Multiplies every roll, on top of ATT. Enchantments set it.
  double damage_bonus = 1;
  std::vector<std::string> unsupported;

  /// Null if the weapon has no projectile with that id.
  const Projectile* FindProjectile(int projectile_id) const;
  /// Sum of the weapon's IncrementStat bonuses for a client stat name like "ATT".
  int StatBonusFor(std::string_view stat) const;
};

struct Ability {
  int type = 0;
  std::string id;
  std::string name;
  int slot_type = 0;
  std::optional<int> tier;
  std::optional<double> mp_cost;
  std::optional<double> cooldown;
  std::vector<StatBonus> stat_bonuses;
  std::vector<Projectile> projectiles;
  std::vector<Subattack> subattacks;
  std::vector<Activate> activates;
  /// Projectiles of other items, by type, that activates name in their
  /// `type` parameter.
  std::map<int, Projectile> created_projectiles;
  std::vector<std::string> labels;
  Enchanting enchanting;
  std::string reskin_of;
  std::vector<std::string> unsupported;

  /// The ability's projectiles and subattacks as a weapon, for the hit
  /// estimate and damage rules.
  Weapon AsWeapon() const;
};

/// One change an enchantment makes: the element's name, its text (a
/// number like 1.02 or a kind like IncrementStat) and its attributes.
struct Mutator {
  std::string tag;
  std::optional<double> number;
  std::string text;
  Activate attributes;
};

/// An enchantment fits an item with every `compatible` label and none of
/// the `incompatible` ones. Two can't share an item when one's
/// `incompatible_labels` meets the other's `labels`.
struct Enchantment {
  int type = 0;
  std::string id;
  std::string name;
  std::string description;
  std::vector<std::string> compatible;
  std::vector<std::string> incompatible;
  std::vector<std::string> incompatible_items;
  std::vector<std::string> labels;
  std::vector<std::string> incompatible_labels;
  std::vector<Mutator> mutators;
  /// Items that only hold subattacks for SetSubAttack and AddSubAttack, by id.
  struct Container {
    std::vector<Projectile> projectiles;
    std::vector<Subattack> subattacks;
    std::vector<std::string> unsupported;
  };
  std::map<std::string, Container> containers;
};

/// An equipment set. Slots are 0 weapon, 1 ability, 2 armor, 3 ring; a slot
/// can accept several items.
struct EquipmentSet {
  struct Piece {
    int slot = 0;
    int item_type = 0;
    std::string name;
  };
  /// Stats added once this many slots hold a piece. Bonuses add up.
  struct Bonus {
    int pieces = 0;
    std::vector<StatBonus> stats;
  };
  int type = 0;
  std::string name;
  std::vector<Piece> pieces;
  std::vector<Bonus> bonuses;
  std::vector<std::string> unsupported;
};

/// An armor or a ring.
struct Gear {
  int type = 0;
  std::string id;
  std::string name;
  int slot_type = 0;
  std::optional<int> tier;
  std::vector<StatBonus> stat_bonuses;
  /// What enchantments match against.
  std::vector<std::string> labels;
  Enchanting enchanting;
  std::string reskin_of;
  std::vector<std::string> unsupported;
};

struct Dataset {
  int schema_version = 0;
  std::string build;
  std::vector<Class> classes;
  std::vector<Weapon> weapons;
  std::vector<Ability> abilities;
  std::vector<Gear> armors;
  std::vector<Gear> rings;
  std::vector<EquipmentSet> sets;
  std::vector<Enchantment> enchantments;
  /// Enchantment ids by pool name. Empty in datasets from before pools,
  /// where every item takes up to 4 of any enchantment that fits it.
  std::map<std::string, std::set<std::string>, std::less<>> enchantment_pools;
};

}  // namespace rotmg
