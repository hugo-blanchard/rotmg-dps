#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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

/// The effect parameters the engine reads, by name in the order of
/// kParamNames, so finding one compares a number rather than text.
enum class ParamKey : std::uint8_t {
  kActivationCount, kAmount, kAoeActivationCount, kAoeArmorPierce, kAoeDamage, kAoeMaxTargets, kAoeRange, kAoeTime,
  kArcDegrees, kArcGap, kArmTime, kArmorPierce, kBombCondEffect, kBombCondEffectDuration, kBombCount, kBombDamage,
  kBombDelay, kBombRadius, kBombTriggerCount, kBombTriggerPeriod, kChannel, kCondDuration, kCondEffect, kCooldown,
  kDamageBoost, kDecrDamage, kDuration, kEffect, kGapAngle, kGapTiles, kHpMinThresholdRelative, kHpRequiredRelative,
  kIdMeter, kImpactDamage, kMaxDamage, kMaxDistance, kMaxTargets, kMeterCost, kMinDamage, kMinDistance,
  kMinimumMeter, kMultiplier, kMustNotWear, kMustNotWearOR, kMustWear, kMustWearOR, kNumDead, kNumShots, kObjectId,
  kOffsetAngle, kProc, kRadius, kRange, kRequiredConditions, kRequiredConditionsNOR, kScalingStat, kSensitivity, kStat, kStatModActivationCount,
  kStatModBombCondDuration, kStatModBombCount, kStatModBombDamage, kStatModBombRadius, kStatModDamage,
  kStatModDamageBoost, kStatModDuration, kStatModImpactDamage, kStatModNumShots, kStatModPerTarget, kStatModRadius,
  kStatModScalingMin, kStatModTrapCondDuration, kStatModTrapDamage, kStatModTrapRadius, kStatRelativeTo,
  kSubAttackIndex, kSubAttackObject, kTargetMouse, kThrowTime, kTime, kTotalDamage, kType, kUndeadId, kWisDamageBase, kWisMin,
  kWisPerDuration, kWisPerTarget,
  kOther,
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(ParamKey::kOther)> kParamNames = {
    "activationCount", "amount", "aoeActivationCount", "aoeArmorPierce", "aoeDamage", "aoeMaxTargets", "aoeRange",
    "aoeTime", "arcDegrees", "arcGap", "armTime", "armorPierce", "bombCondEffect", "bombCondEffectDuration",
    "bombCount", "bombDamage", "bombDelay", "bombRadius", "bombTriggerCount", "bombTriggerPeriod", "channel",
    "condDuration", "condEffect", "cooldown", "damageBoost", "decrDamage", "duration", "effect", "gapAngle",
    "gapTiles", "hpMinThresholdRelative", "hpRequiredRelative", "idMeter", "impactDamage", "maxDamage",
    "maxDistance", "maxTargets", "meterCost", "minDamage", "minDistance", "minimumMeter", "multiplier",
    "mustNotWear", "mustNotWearOR", "mustWear", "mustWearOR", "numDead", "numShots", "objectId", "offsetAngle", "proc",
    "radius", "range", "requiredConditions", "requiredConditionsNOR", "scalingStat", "sensitivity", "stat", "statModActivationCount", "statModBombCondDuration",
    "statModBombCount", "statModBombDamage", "statModBombRadius", "statModDamage", "statModDamageBoost",
    "statModDuration", "statModImpactDamage", "statModNumShots", "statModPerTarget", "statModRadius",
    "statModScalingMin", "statModTrapCondDuration", "statModTrapDamage", "statModTrapRadius", "statRelativeTo",
    "subAttackIndex", "subAttackObject", "targetMouse", "throwTime", "time", "totalDamage", "type", "undeadId", "wisDamageBase",
    "wisMin", "wisPerDuration", "wisPerTarget"};

/// The key for a parameter name, or kOther for one the engine doesn't read.
ParamKey ParamKeyOf(std::string_view name);

/// An effect's parameters, by name and in name order like a map, each with
/// its ParamKey.
class Params {
 public:
  using Value = std::variant<double, bool, std::string>;
  using Entry = std::pair<std::string, Value>;
  using const_iterator = std::vector<Entry>::const_iterator;

  Params() = default;
  Params(std::initializer_list<Entry> entries) { *this = entries; }
  Params& operator=(std::initializer_list<Entry> entries) {
    entries_.clear();
    keys_.clear();
    for (const Entry& e : entries) {
      insert_or_assign(e.first, e.second);
    }
    return *this;
  }

  const_iterator begin() const { return entries_.begin(); }
  const_iterator end() const { return entries_.end(); }
  std::size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }
  const_iterator find(std::string_view name) const {
    const auto it = Position(name);
    return it != entries_.end() && it->first == name ? it : entries_.end();
  }

  /// Adds the parameter unless the effect has it already.
  template <typename V>
  void emplace(std::string_view name, V&& value) {
    if (find(name) == end()) {
      Insert(name, Value(std::forward<V>(value)));
    }
  }
  void insert_or_assign(std::string_view name, Value value) { (*this)[name] = std::move(value); }
  /// The parameter, added as 0 when the effect hasn't got it.
  Value& operator[](std::string_view name) {
    const auto it = Position(name);
    if (it != entries_.end() && it->first == name) {
      return entries_[static_cast<std::size_t>(it - entries_.begin())].second;
    }
    return Insert(name, Value(0.0));
  }

  /// The parameter with this key, or null.
  const Value* Find(ParamKey key) const {
    for (std::size_t i = 0; i < keys_.size(); ++i) {
      if (keys_[i] == key) {
        return &entries_[i].second;
      }
    }
    return nullptr;
  }

 private:
  const_iterator Position(std::string_view name) const {
    return std::ranges::lower_bound(entries_, name, std::less<>{}, &Entry::first);
  }
  Value& Insert(std::string_view name, Value value) {
    const auto at = Position(name) - entries_.begin();
    keys_.insert(keys_.begin() + at, ParamKeyOf(name));
    return entries_.insert(entries_.begin() + at, Entry(std::string(name), std::move(value)))->second;
  }

  std::vector<Entry> entries_;
  std::vector<ParamKey> keys_;
};

/// One effect of using an item, with the client's parameters as they are:
/// numbers, booleans or strings.
struct Activate {
  using Value = Params::Value;

  std::string type;
  Params params;

  std::optional<double> Number(ParamKey key) const;
  std::optional<double> Number(std::string_view key) const;
  /// Empty when the parameter is missing or isn't text. Valid while the
  /// effect is.
  std::string_view Text(ParamKey key) const;
  std::string_view Text(std::string_view key) const;
  /// False when the parameter is missing or isn't a boolean.
  bool Flag(ParamKey key) const;
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
  /// OnEnemyHitActivate effects: a chance on each enemy a shot hits. A
  /// SpawnCreep that explodes has minDamage, maxDamage and radius too.
  std::vector<Activate> on_enemy_hit;
  /// OnPlayerAbilityActivate effects, which go off when the ability is used.
  std::vector<Activate> on_ability;
  /// On-equip effects other than flat stats, like IncrementStatRelative.
  std::vector<Activate> on_equip;
  /// Projectiles of other items, by type, that on-shoot and on-hit effects
  /// create.
  std::map<int, Projectile> created_projectiles;
  /// The item's labels, which enchantments match against.
  std::vector<std::string> labels;
  Enchanting enchanting;
  /// The item this one plays exactly like, when it's a reskin of it.
  std::string reskin_of;
  /// The item this one plays exactly like apart from the enchantments it
  /// can take, like Cupid's Bow, which also takes Valentine enchantments.
  std::string variant_of;
  /// No player can get it any more, like the Tier 8 abilities.
  bool unobtainable = false;
  /// Multiplies every roll, on top of ATT. Enchantments set it.
  double damage_bonus = 1;
  std::vector<std::string> unsupported;

  /// Null if the weapon has no projectile with that id.
  const Projectile* FindProjectile(int projectile_id) const;
  /// Sum of the weapon's IncrementStat bonuses for a client stat name like "ATT".
  int StatBonusFor(std::string_view stat) const;
};

/// One way of using an ability the player switches between, like a
/// Druid's normal ability and its form: what it costs and what it does when
/// used, when its holder shoots, when they switch to it, when they hit an
/// enemy and when they heal.
struct AbilityMode {
  std::string name;
  /// The ability's subattack its shots use.
  std::optional<int> attack_index;
  std::optional<double> mp_cost;
  std::optional<double> cooldown;
  std::optional<double> mp_cost_per_second;
  std::optional<double> mp_end_cost;
  /// How long the mode lasts once switched to, in seconds.
  std::optional<double> ability_time;
  std::optional<double> switch_time;
  bool multi_phase = false;
  std::vector<Activate> activates;
  std::vector<Activate> on_shoot;
  std::vector<Activate> on_ability;
  std::vector<Activate> on_switch;
  std::vector<Activate> on_enemy_hit;
  std::vector<Activate> on_heal;
};

/// An item's bar that its modes fill and spend.
struct Meter {
  int id = 0;
  double max_value = 0;
  double start_value = 0;
};

/// An object a RaiseDead effect raises, and how it fights for its lifetime:
/// `attacks_per_second` attacks of `shots` of each projectile, `arc_gap`
/// degrees apart and centred on the enemy it aims at, or, with `bursts`, that many bursts where it was raised, the k-th from
/// the k-th projectile and reaching the k-th radius in tiles (both cycling).
struct Undead {
  std::vector<Projectile> projectiles;
  int shots = 1;
  double arc_gap = 0;
  double attacks_per_second = 0;
  int bursts = 0;
  std::vector<double> burst_radii;
  double lifetime_s = 0;

  /// Attacks or bursts over its lifetime. An attack comes one attack's time
  /// after the last, the first after it's raised, so only whole ones count.
  int Attacks() const;
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
  /// OnPlayerShootActivate effects, which go off as the weapon fires.
  std::vector<Activate> on_shoot;
  /// OnPlayerAbilityActivate effects, which go off when it's used. A build's
  /// ability carries those of the other gear worn too.
  std::vector<Activate> on_ability;
  /// On-equip effects other than flat stats, like IncrementStatRelative.
  std::vector<Activate> on_equip;
  /// Projectiles of other items, by type, that activates and other effects
  /// name in their `type` parameter.
  std::map<int, Projectile> created_projectiles;
  /// The objects RaiseDead effects raise, by id.
  std::map<std::string, Undead, std::less<>> undead;
  /// The ways of using the ability the player switches between, if it has
  /// several. Until switching is modelled, it's used in its first: the
  /// fields above hold that mode's effects and costs, and `subattacks` only
  /// its subattack, with every mode's in `mode_subattacks`.
  std::vector<AbilityMode> modes;
  std::vector<Subattack> mode_subattacks;
  std::optional<Meter> meter;
  std::vector<std::string> labels;
  Enchanting enchanting;
  std::string reskin_of;
  std::string variant_of;
  bool unobtainable = false;
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
  /// The pieces whose own setType names the set. When there are any, the set
  /// only counts while one of them is worn. Empty in older datasets.
  std::vector<int> key_pieces;
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
  /// OnPlayerShootActivate effects, which go off as the weapon fires.
  std::vector<Activate> on_shoot;
  /// OnPlayerAbilityActivate effects, which go off when the ability is used.
  std::vector<Activate> on_ability;
  /// On-equip effects other than flat stats, like IncrementStatRelative.
  std::vector<Activate> on_equip;
  /// Projectiles of other items, by type, that the effects name.
  std::map<int, Projectile> created_projectiles;
  Enchanting enchanting;
  std::string reskin_of;
  std::string variant_of;
  bool unobtainable = false;
  std::vector<std::string> unsupported;
};

/// Hashes strings and string views alike, so a map keyed by strings can be
/// searched with a view.
struct StringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};
using NameIndex = std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>>;

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
  /// Positions in the lists above, by item name and enchantment id, filled
  /// by ParseDataset. Lookups search a list itself when its index doesn't
  /// cover it, as in a dataset built or changed by hand.
  struct Index {
    NameIndex weapons, abilities, armors, rings, enchantments;
  } index;
};

}  // namespace rotmg
