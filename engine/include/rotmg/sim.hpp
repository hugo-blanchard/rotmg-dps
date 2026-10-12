#pragma once

#include <cstddef>
#include <deque>
#include <vector>

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
/// DEF and statuses, and how it moves.
struct SimTarget {
  Point position;
  double radius = kDefaultTargetRadius;
  Target target;
  Movement movement{};
};

/// Where everything starts, in tiles. The player aims at the centre of
/// `targets[aim]` each time it fires.
struct Scenario {
  Point player;
  std::vector<SimTarget> targets;
  std::size_t aim = 0;
  Movement player_movement{};
  /// Seeds anything random, like wandering targets.
  unsigned long long seed = 1;
};

/// The player's speed in tiles per second: 4 + 5.6 * SPD / 75 (RealmEye:
/// Character Stats).
double MovementSpeed(int speed);

/// A deterministic, fixed-step simulation of a character firing a weapon at
/// one or more round targets. Projectiles move with ProjectilePath, in its
/// frame turned to face the aim and moved to the player, so a player and a
/// single target that both stand still give exactly the hit estimate: once
/// every shot of a volley has landed or expired, its hits equal the
/// estimate's. Damage per hit is the expected damage against the target
/// hit, so the long-run DPS matches EvaluateWeapon's.
class Simulation {
 public:
  struct Shot {
    int subattack = 0;
    int projectile_id = 0;
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
    double damage = 0;
  };

  Simulation(const Weapon& weapon, const Character& character, const Scenario& scenario);
  /// One still target `distance` tiles straight ahead of a still player.
  Simulation(const Weapon& weapon, const Character& character, const Target& target, double distance,
             double radius);
  // Shots point into the weapon this object owns, so it can't be copied or moved.
  Simulation(const Simulation&) = delete;
  Simulation& operator=(const Simulation&) = delete;

  /// Moves time forward in whole kStepMs steps. Leftover time carries over to
  /// the next call, so many small calls add up to the same as one big one.
  void Advance(double ms);

  double TimeMs() const { return static_cast<double>(steps_) * kStepMs; }
  const std::vector<Shot>& Shots() const { return shots_; }
  const Counters& Totals() const { return totals_; }
  const std::vector<Counters>& PerSubattack() const { return per_subattack_; }
  const std::vector<TargetCounters>& PerTarget() const { return per_target_; }
  /// Where everything is now.
  const Scenario& World() const { return world_; }
  /// The player's speed, from SPD, in tiles per second.
  double PlayerSpeed() const { return player_speed_; }
  /// The aim target's centre and radius.
  Point TargetCenter() const;
  double Radius() const;

 private:
  struct Clock {
    double interval_ms = 0;
    double next_ms = 0;
    bool burst = false;
    int burst_count = 0;
    double burst_delay_ms = 0;
    int burst_fired = 0;
    double burst_start_ms = 0;
  };
  struct Live {
    int subattack = 0;
    int index = 0;
    long volley = 0;
    int step = 0;
    // Where it was fired from and which way, in radians from +y turning
    // right, which place its path in the world.
    Point origin;
    double aim = 0;
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

  void Step();
  void Move();
  void Fire(int subattack);
  void Land(Live& shot);
  void Retire(const Live& shot);
  void ResolveFinished();
  Point WorldAt(const Live& shot, int step) const;

  const Weapon weapon_;
  // Where things started, and where they are now.
  const Scenario start_;
  Scenario world_;
  double player_speed_ = 0;
  // Wandering targets: where each is heading, and the random state.
  std::vector<Point> wander_to_;
  unsigned long long random_ = 0;
  long steps_ = 0;
  double carry_ms_ = 0;
  long next_volley_ = 0;
  std::vector<Clock> clocks_;
  // Expected damage per hit, by subattack, then target.
  std::vector<std::vector<double>> damage_per_hit_;
  std::vector<bool> pierces_;
  std::vector<std::vector<ProjectilePath>> paths_;
  std::vector<Live> live_;
  std::deque<OpenVolley> open_;
  std::vector<Shot> shots_;
  Counters totals_;
  std::vector<Counters> per_subattack_;
  std::vector<TargetCounters> per_target_;
};

}  // namespace rotmg
