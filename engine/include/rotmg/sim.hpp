#pragma once

#include <deque>
#include <vector>

#include "rotmg/evaluate.hpp"
#include "rotmg/path.hpp"

namespace rotmg {

/// A deterministic, fixed-step simulation of a character firing a weapon at
/// a stationary round target `distance` tiles away. Projectiles move with
/// ProjectilePath, the same motion the hit estimate uses, so once every shot
/// of a volley has landed or expired, the hits counted for it equal the
/// estimate's. Damage per hit is the expected damage, so the long-run DPS
/// matches EvaluateWeapon's.
class Simulation {
 public:
  struct Shot {
    int subattack = 0;
    int projectile_id = 0;
    Point position;
    double heading_degrees = 0;
    double age_ms = 0;
    /// Piercing shots keep flying after a hit, but never hit the target twice.
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

  Simulation(const Weapon& weapon, const Character& character, const Target& target, double distance,
             double radius);
  // Shots point into the weapon this object owns, so it can't be copied or moved.
  Simulation(const Simulation&) = delete;
  Simulation& operator=(const Simulation&) = delete;

  /// Moves time forward in whole kStepMs steps. Leftover time carries over to
  /// the next call, so many small calls add up to the same as one big one.
  void Advance(double ms);

  double TimeMs() const { return steps_ * kStepMs; }
  const std::vector<Shot>& Shots() const { return shots_; }
  const Counters& Totals() const { return totals_; }
  const std::vector<Counters>& PerSubattack() const { return per_subattack_; }
  Point TargetCenter() const { return {0, distance_}; }
  double Radius() const { return radius_; }

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
    bool hit = false;
  };
  struct OpenVolley {
    long id = 0;
    int subattack = 0;
    int remaining = 0;
    int hits = 0;
  };

  void Step();
  void Fire(int subattack);
  void Land(Live& shot);
  void Retire(const Live& shot);
  void ResolveFinished();

  const Weapon weapon_;
  double distance_;
  double radius_;
  long steps_ = 0;
  double carry_ms_ = 0;
  long next_volley_ = 0;
  std::vector<Clock> clocks_;
  std::vector<double> damage_per_hit_;
  std::vector<bool> pierces_;
  std::vector<std::vector<ProjectilePath>> paths_;
  std::vector<Live> live_;
  std::deque<OpenVolley> open_;
  std::vector<Shot> shots_;
  Counters totals_;
  std::vector<Counters> per_subattack_;
};

}  // namespace rotmg
