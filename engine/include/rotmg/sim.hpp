#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "rotmg/ability.hpp"
#include "rotmg/evaluate.hpp"
#include "rotmg/path.hpp"

namespace rotmg {

/// How something moves, in tiles and tiles per second.
///
/// Targets move around where they start, `speed` tiles per second: kStrafe
/// goes side to side across the line to the player, `range` tiles each way;
/// kCircle goes round a circle of radius `range` that passes through its
/// start; kChargeRetreat walks towards the player and back, between `range`
/// and `far` tiles from it; kWander walks to random points within `range`
/// tiles of its start, seeded so a run can be repeated.
///
/// The player moves relative to the target it aims at, at the speed its SPD
/// gives: kKeepDistance holds `range` tiles away; kOrbit circles it at
/// `range` tiles; kKite walks between `range` and `far` tiles away; kStrafe
/// goes side to side across the line to it, `range` tiles each way.
struct Movement {
  enum class Kind { kStill, kStrafe, kCircle, kChargeRetreat, kWander, kKeepDistance, kOrbit, kKite };
  Kind kind = Kind::kStill;
  /// Targets only; the player's comes from SPD.
  double speed = 0;
  double range = 0;
  double far = 0;
  bool clockwise = true;
};

/// A target in the simulation: where its centre starts, how big it is, its
/// DEF and statuses, how it moves, and its hit points (0 for a dummy that
/// can't die). It can go invulnerable for `invulnerable_s` seconds every
/// `invulnerable_every_s`, the first time that long after the start, like a
/// boss between phases.
struct SimTarget {
  Point position;
  double radius = kDefaultTargetRadius;
  Target target;
  Movement movement{};
  double hp = 0;
  double invulnerable_every_s = 0;
  double invulnerable_s = 0;
};

/// Where everything starts, in tiles. The player aims at the centre of
/// `targets[aim]` each time it fires, and casts at it: that centre is the
/// cursor.
struct Scenario {
  Point player;
  std::vector<SimTarget> targets;
  std::size_t aim = 0;
  Movement player_movement{};
  /// Seeds anything random, like wandering targets.
  unsigned long long seed = 1;
  /// Whether the MP bar starts full, or empty for the steady state.
  bool full_mp = true;
  /// Whether a target that dies comes straight back with full HP, for
  /// steady-state numbers, or stays dead.
  bool respawn = false;
  /// Roll each hit's damage and each proc's chance from the seed rather than
  /// using averages, to show how runs vary.
  bool random_rolls = false;
  /// Aim the weapon where a moving target will be when the shot gets there,
  /// as a player leading their shots would, rather than where it is.
  bool lead = false;
};

/// The player's speed in tiles per second: 4 + 5.6 * SPD / 75 (RealmEye:
/// Character Stats).
double MovementSpeed(int speed);

/// A deterministic, fixed-step simulation of a character firing a weapon,
/// and casting its ability, at one or more round targets.
///
/// Projectiles move with ProjectilePath, in its frame turned to face the aim
/// and moved to where they were fired from, so a player and a single target
/// that both stand still give exactly the hit estimate: once every shot of a
/// volley has landed or expired, its hits equal the estimate's. Damage per
/// hit is the expected damage against the target hit, with the stats and
/// statuses of the moment, so long runs match the analytic DPS.
///
/// The ability is cast at the aim target whenever MP and its cooldown allow.
/// Its buffs change the weapon from the next shot; its debuffs and
/// projectiles' status effects land on the targets they reach. The weapon's
/// on-shoot procs go off on its fastest subattack's shots: poison bursts,
/// extra shots at the cursor and chance boosts, the chance spread evenly over
/// shots so a run doesn't depend on luck.
class Simulation {
 public:
  /// What made a shot or did some damage: the weapon's projectiles, the
  /// ability, or the weapon's on-shoot procs.
  enum class From { kWeapon, kAbility, kProc };

  struct Shot {
    /// The weapon subattack it came from, or -1 for the ability's and procs'.
    int subattack = 0;
    From from = From::kWeapon;
    int projectile_id = 0;
    std::string object_id;
    Point position;
    /// Degrees from straight up (+y), turning right.
    double heading_degrees = 0;
    double age_ms = 0;
    /// Whether it has hit anything yet.
    bool hit = false;
  };

  struct Counters {
    long volleys = 0;
    long projectiles = 0;
    long hits = 0;
    double damage = 0;
    /// Volleys whose shots have all landed or expired, and their hits. Their
    /// ratio is the measured hits per volley.
    long resolved_volleys = 0;
    long resolved_hits = 0;
  };

  struct TargetCounters {
    long hits = 0;
    /// All damage taken, the ability's included. Damage past a target's
    /// last hit point doesn't count.
    double damage = 0;
    double ability_damage = 0;
    /// Hit points left, for a target that can die.
    double hp = 0;
    long kills = 0;
    /// When it first died, or -1.
    double killed_ms = -1;
  };

  /// Damage done to one target, for showing as a number.
  struct HitEvent {
    std::size_t target = 0;
    double damage = 0;
    From from = From::kWeapon;
  };

  /// The history's resolution.
  static constexpr double kHistoryMs = 100;

  struct AbilityCounters {
    long casts = 0;
    long hits = 0;
    double damage = 0;
  };

  /// Something an ability left in the world, for drawing.
  struct Effect {
    enum class Kind { kPoison, kTrap, kBlast, kBolt };
    Kind kind = Kind::kBlast;
    Point position;
    double radius = 0;
    double until_ms = 0;
    /// Lightning: every target the bolt went through, in order.
    std::vector<Point> chain;
    /// Traps: whether it's waiting for a target.
    bool armed = false;
  };

  /// `ability` is cast as often as it can be; null for none.
  Simulation(const Weapon& weapon, const Character& character, const Scenario& scenario,
             const Ability* ability = nullptr);
  /// One still target `distance` tiles straight ahead of a still player.
  Simulation(const Weapon& weapon, const Character& character, const Target& target, double distance,
             double radius);
  // Shots point into the weapon and ability this object owns, so it can't be
  // copied or moved.
  Simulation(const Simulation&) = delete;
  Simulation& operator=(const Simulation&) = delete;

  /// Moves time forward in whole kStepMs steps. Leftover time carries over to
  /// the next call, so many small calls add up to the same as one big one.
  void Advance(double ms);

  double TimeMs() const { return static_cast<double>(steps_) * kStepMs; }
  const std::vector<Shot>& Shots() const { return shots_; }
  /// The weapon's shots only.
  const Counters& Totals() const { return totals_; }
  const std::vector<Counters>& PerSubattack() const { return per_subattack_; }
  const std::vector<TargetCounters>& PerTarget() const { return per_target_; }
  const AbilityCounters& AbilityTotals() const { return ability_totals_; }
  /// The weapon's on-shoot procs, with how often they went off in `casts`.
  const AbilityCounters& ProcTotals() const { return proc_totals_; }
  /// Hits and blasts since the last Advance, at most 256; poison's steady
  /// damage isn't in them.
  const std::vector<HitEvent>& RecentHits() const { return recent_hits_; }
  /// Damage done in each kHistoryMs since the start, and MP at the end of
  /// each.
  const std::vector<double>& DamageHistory() const { return damage_history_; }
  const std::vector<double>& MpHistory() const { return mp_history_; }
  const std::vector<Effect>& Effects() const { return effects_; }
  /// Where everything is now.
  const Scenario& World() const { return world_; }
  /// The player's speed, from SPD, in tiles per second.
  double PlayerSpeed() const { return player_speed_; }
  double Mp() const { return mp_; }
  double MaxMp() const { return max_mp_; }
  /// Statuses and stat boosts up now, like "Berserk" or "+10 ATT".
  std::vector<std::string> PlayerStatuses() const;
  std::vector<std::string> TargetStatuses(std::size_t target) const;
  /// How long each status or stat boost has been up so far, in ms: on the
  /// player by effect ("Berserk", "ATT"), and on one target.
  const std::map<std::string, double>& PlayerUptime() const { return player_uptime_; }
  const std::map<std::string, double>& TargetUptime(std::size_t target) const { return target_uptime_[target]; }
  /// Whether a target is in one of its invulnerable windows now.
  bool Invulnerable(std::size_t target) const;
  /// The aim target's centre and radius.
  Point TargetCenter() const;
  double Radius() const;

 private:
  struct Clock {
    std::size_t subattack = 0;
    double next_ms = 0;
    int burst_fired = 0;
    double burst_start_ms = 0;
  };
  // Shots that share a projectile and a pattern.
  struct Group {
    const Projectile* projectile = nullptr;
    std::vector<ProjectilePath> paths;
    /// The weapon subattack, or -1 for the ability's and procs' shots.
    int subattack = -1;
    /// The flat StatMod damage on each hit of the ability's and procs' shots.
    double bonus = 0;
    From from = From::kWeapon;
  };
  struct Live {
    std::size_t group = 0;
    int index = 0;
    long volley = 0;
    int step = 0;
    // Where it was fired from and which way, in radians from +y turning
    // right, which place its path in the world.
    Point origin;
    double aim = 0;
    // ATT, Damaging and the like when it was fired.
    Multiplier multiplier;
    // Targets it has hit, by index; each only once.
    std::vector<bool> hit;
    bool stopped = false;
  };
  struct OpenVolley {
    long id = 0;
    int subattack = 0;
    int remaining = 0;
    int hits = 0;
  };
  // A status or stat boost that runs out.
  struct Timed {
    std::string effect;
    int amount = 0;
    double until_ms = 0;
    // When it starts to count.
    double from_ms = 0;
    // Boosts of one effect on the same channel don't stack: the larger
    // amount holds until the later end.
    std::string channel{};
    // A damage boost: what damage taken is multiplied by.
    double factor = 1;
  };
  // Something an ability set going that happens later.
  struct Pending {
    enum class Kind { kNovaBlast, kPoisonLands, kShockblast, kStickyBomb };
    Kind kind = Kind::kNovaBlast;
    double at_ms = 0;
    Point centre;
    double radius = 0;
    const Activate* activate = nullptr;
    From from = From::kAbility;
    // A shockblast or sticky bomb: the target it's on, which it follows.
    std::size_t source = 0;
  };
  struct Poison {
    std::vector<std::size_t> targets;
    double per_step = 0;
    // Damage still to come, before Curse.
    double left = 0;
    From from = From::kAbility;
  };
  // One of the weapon's on-shoot effects.
  struct Proc {
    const Activate* effect = nullptr;
    OnShootModel model = OnShootModel::kNone;
    // Extra shots: their group.
    std::size_t group = 0;
    double ready_ms = 0;
    // Chance boosts: the chance gathered over shots since the cooldown.
    double chance = 0;
  };
  // One of the weapon's on-hit effects.
  struct HitProc {
    const Activate* effect = nullptr;
    OnHitModel model = OnHitModel::kNone;
    // Extra shots: their group.
    std::size_t group = 0;
    double ready_ms = 0;
    // The chance gathered over hits since the cooldown.
    double chance = 0;
  };
  // An on-use effect of the ability or the other gear.
  struct OnCast {
    const Activate* effect = nullptr;
    int group = -1;
    double ready_ms = 0;
    // The chance gathered over casts since the cooldown.
    double chance = 0;
  };
  struct Trap {
    Point at;
    double lands_ms = 0;
    double armed_ms = 0;
    double expires_ms = 0;
    double radius = 0;
    double trigger = 0;
    const Activate* activate = nullptr;
  };

  void Step();
  void Move();
  void Fire(std::size_t subattack);
  void Cast();
  // One effect of a cast, with the group its shots go in or -1, and the
  // channel its buff goes on.
  void CastEffect(const Activate& effect, int group, const std::string& channel);
  // A new group for an effect's shots, or -1 for an effect without any.
  int ShotGroup(const Activate& effect);
  void OnShoot();
  // Rolls the on-hit effects for the weapon hits since the last roll.
  void RollHitProcs();
  // An on-hit explosion centred on `at`, hurting every target in its radius.
  void Explode(const Activate& effect, Point at);
  // Adds the shots a BulletNova or BulletCreate makes, with this projectile,
  // and returns the group.
  std::size_t AddGroup(const Projectile& projectile, const Activate& effect, From from);
  // Places a BulletCreate's shots around the cursor.
  void CreateShots(std::size_t group, const Activate& effect);
  void Boost(const std::string& effect, int amount, double duration_s, const std::string& channel);
  void Spawn(std::size_t group, int index, Point origin, double aim, long volley);
  void Land(Live& shot);
  void Retire(const Live& shot);
  void ResolveFinished();
  void RunPending();
  void TickPoisons();
  void CheckTraps();
  void Explode(const Trap& trap);
  void Debuff(std::size_t target, const std::string& effect, double duration_s, double delay_ms = 0,
              double factor = 1);
  // Deals damage, at most what the target has left, and returns what was
  // dealt. `shown` puts it in RecentHits; `always` deals it even while the
  // target is invulnerable.
  double Damage(std::size_t target, double amount, From from, bool shown = true, bool always = false);
  bool Alive(std::size_t target) const;
  // Aims at the closest target alive when the one aimed at has died.
  void Retarget();
  // A whole number from low to high, both included.
  int Roll(int low, int high);
  Point WorldAt(const Live& shot, int step) const;
  double AimAngle() const;
  // Where to aim a shot from this group, leading the target if asked.
  double FiringAngle(const Group& group) const;
  // The character with the boosts and statuses up now.
  Character Current() const;
  // A target with its own statuses and the debuffs on it now.
  Target TargetNow(std::size_t target) const;
  double HitDamage(const Group& group, Multiplier multiplier, std::size_t target);
  double AbilityHit(int low, int high, double bonus, std::size_t target, bool armor_piercing);

  const Weapon weapon_;
  const std::optional<Ability> ability_;
  // Where things started, and where they are now.
  const Scenario start_;
  Scenario world_;
  // The character with gear but no buffs.
  Character base_;
  double player_speed_ = 0;
  std::vector<Point> wander_to_;
  // Each target's velocity over the last step, in tiles per second.
  std::vector<Point> velocity_;
  unsigned long long random_ = 0;
  long steps_ = 0;
  double carry_ms_ = 0;
  long next_volley_ = 0;
  std::vector<Clock> clocks_;
  std::vector<Group> groups_;
  // Group of each ability activate that makes shots, by activate index, or -1.
  std::vector<int> activate_group_;
  std::vector<std::size_t> shoot_groups_;
  std::vector<Live> live_;
  std::deque<OpenVolley> open_;
  std::vector<Shot> shots_;
  Counters totals_;
  std::vector<Counters> per_subattack_;
  std::vector<TargetCounters> per_target_;
  AbilityCounters ability_totals_;
  AbilityCounters proc_totals_;
  // Damage rolls and proc chances, apart from movement's randomness so
  // turning rolls on doesn't change where things go.
  unsigned long long rolls_ = 0;
  std::vector<HitEvent> recent_hits_;
  std::map<std::string, double> player_uptime_;
  std::vector<std::map<std::string, double>> target_uptime_;
  std::vector<double> damage_history_;
  std::vector<double> mp_history_;
  double bucket_damage_ = 0;
  std::vector<Proc> procs_;
  std::vector<HitProc> hit_procs_;
  // Targets the weapon's shots hit since on-hit effects last rolled, once
  // per projectile.
  std::vector<std::size_t> weapon_hits_;
  std::vector<OnCast> on_cast_;
  // The subattack whose shots set off procs.
  std::size_t proc_subattack_ = 0;
  double mp_ = 0;
  double max_mp_ = 0;
  double mp_cost_ = 0;
  double cooldown_ms_ = 0;
  double next_cast_ms_ = 0;
  std::vector<Timed> player_buffs_;
  std::vector<std::vector<Timed>> target_debuffs_;
  std::vector<Pending> pending_;
  std::vector<Poison> poisons_;
  std::vector<Trap> traps_;
  std::vector<Effect> effects_;
  std::map<std::tuple<const Projectile*, int, int, int, int, double>, double> damage_cache_;
};

}  // namespace rotmg
