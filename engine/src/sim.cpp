#include "rotmg/sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "rotmg/damage.hpp"
#include "rotmg/evaluate.hpp"
#include "rotmg/fire_rate.hpp"

namespace rotmg {
namespace {

Scenario OneTarget(const Target& target, double distance, double radius) {
  return {{0, 0}, {{{0, distance}, radius, target, {}}}, 0, {}, 1, true};
}

// Goes from 0 up to `span` and back down as `travelled` grows.
double Bounce(double travelled, double span) {
  if (span <= 0) {
    return 0;
  }
  const double phase = std::fmod(std::max(0.0, travelled), 2 * span);
  return phase < span ? phase : 2 * span - phase;
}

Point Add(Point a, Point v, double k = 1) {
  return {a.x + v.x * k, a.y + v.y * k};
}

double Distance(Point a, Point b) {
  return std::hypot(b.x - a.x, b.y - a.y);
}

// The unit vector from a to b, or straight up when they're the same point.
Point Towards(Point a, Point b) {
  const double d = Distance(a, b);
  return d > 0 ? Point{(b.x - a.x) / d, (b.y - a.y) / d} : Point{0, 1};
}

// splitmix64: small, fast and the same everywhere, so seeded runs repeat.
// Returns a value in [0, 1).
double NextRandom(unsigned long long& state) {
  unsigned long long z = (state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z ^= z >> 31;
  return static_cast<double>(z >> 11) / static_cast<double>(1ULL << 53);
}

// How far along the segment from a to b, from 0 to 1, a shot first touches a
// circle, or nothing if it doesn't. A segment of zero length is a point.
std::optional<double> Entry(Point a, Point b, Point centre, double radius) {
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double length2 = dx * dx + dy * dy;
  const double t = length2 > 0 ? std::clamp(((centre.x - a.x) * dx + (centre.y - a.y) * dy) / length2, 0.0, 1.0) : 0;
  const double ex = a.x + t * dx - centre.x;
  const double ey = a.y + t * dy - centre.y;
  const double miss2 = ex * ex + ey * ey;
  if (miss2 > radius * radius) {
    return std::nullopt;
  }
  if (length2 == 0) {
    return 0.0;
  }
  // Back from the closest point to where the segment crosses the circle.
  return std::max(0.0, t - std::sqrt((radius * radius - miss2) / length2));
}

}  // namespace

double MovementSpeed(int speed) {
  return 4 + 5.6 * speed / 75.0;
}

void Simulation::Move() {
  const double t = TimeMs() / 1000;
  const double dt = kStepMs / 1000;
  for (std::size_t i = 0; i < world_.targets.size(); ++i) {
    const SimTarget& from = start_.targets[i];
    const Movement& m = from.movement;
    Point& at = world_.targets[i].position;
    // Away from where the player started, and its right.
    const Point out = Towards(start_.player, from.position);
    const Point side{out.y, -out.x};
    const double travelled = m.speed * t;
    switch (m.kind) {
      case Movement::Kind::kStrafe:
        // From the middle, right first.
        at = Add(from.position, side, Bounce(travelled + m.range, 2 * m.range) - m.range);
        break;
      case Movement::Kind::kCircle: {
        // Round a centre `range` tiles further out, so it starts where it
        // was placed and sets off to the right (or left).
        const double a = m.range > 0 ? travelled / m.range * (m.clockwise ? 1 : -1) : 0;
        const Point centre = Add(from.position, out, m.range);
        at = Add(Add(centre, out, -m.range * std::cos(a)), side, m.range * std::sin(a));
        break;
      }
      case Movement::Kind::kChargeRetreat: {
        // In towards `range` tiles from the player, out to `far`, and so on.
        const double far = std::max(m.far, m.range);
        const double start = std::clamp(Distance(start_.player, from.position), m.range, far);
        at = Add(start_.player, out, far - Bounce(travelled + (far - start), far - m.range));
        break;
      }
      case Movement::Kind::kWander: {
        Point& goal = wander_to_[i];
        const double left = Distance(at, goal);
        if (left <= m.speed * dt) {
          at = goal;
          goal = Add(from.position, {(NextRandom(random_) * 2 - 1), (NextRandom(random_) * 2 - 1)}, m.range);
        } else {
          at = Add(at, Towards(at, goal), m.speed * dt);
        }
        break;
      }
      default:
        break;
    }
  }

  // The player moves after the targets, relative to the one it aims at.
  const Movement& m = start_.player_movement;
  if (m.kind == Movement::Kind::kStill || world_.targets.empty()) {
    return;
  }
  Point& me = world_.player;
  const Point target = TargetCenter();
  const double step = player_speed_ * dt;
  const Point away = Towards(target, me);
  const double distance = Distance(target, me);
  const auto walk_to = [&](Point goal) {
    const double d = Distance(me, goal);
    me = d <= step ? goal : Add(me, Towards(me, goal), step);
  };
  const Point start_away = Towards(start_.targets[start_.aim].position, start_.player);
  const double start_distance = Distance(start_.targets[start_.aim].position, start_.player);
  switch (m.kind) {
    case Movement::Kind::kKeepDistance:
      walk_to(Add(target, away, m.range));
      break;
    case Movement::Kind::kOrbit: {
      // A step along the circle, then back towards its radius.
      const Point along = m.clockwise ? Point{-away.y, away.x} : Point{away.y, -away.x};
      const Point moved = Add(me, along, step);
      const double radius = distance + std::clamp(m.range - distance, -step, step);
      me = Add(target, Towards(target, moved), radius);
      break;
    }
    case Movement::Kind::kKite: {
      // Out to `far`, in to `range`, and so on, timed from the start so it
      // keeps its rhythm when the target moves.
      const double far = std::max(m.far, m.range);
      const double start = std::clamp(start_distance, m.range, far);
      walk_to(Add(target, away, m.range + Bounce(player_speed_ * t + (start - m.range), far - m.range)));
      break;
    }
    case Movement::Kind::kStrafe: {
      // Across the line it started on, keeping its distance.
      const Point side{start_away.y, -start_away.x};
      const Point middle = Add(target, start_away, start_distance);
      walk_to(Add(middle, side, Bounce(player_speed_ * t + m.range, 2 * m.range) - m.range));
      break;
    }
    default:
      break;
  }
}


Simulation::Simulation(const Weapon& weapon, const Character& character, const Target& target, double distance,
                       double radius)
    : Simulation(weapon, character, OneTarget(target, distance, radius)) {}

Simulation::Simulation(const Weapon& weapon, const Character& character, const Scenario& scenario,
                       const Ability* ability)
    : weapon_(weapon),
      ability_(ability ? std::optional<Ability>(*ability) : std::nullopt),
      start_(scenario),
      world_(scenario),
      wander_to_(scenario.targets.size()),
      random_(scenario.seed),
      per_subattack_(weapon.subattacks.size()),
      per_target_(scenario.targets.size()),
      target_debuffs_(scenario.targets.size()) {
  target_uptime_.resize(scenario.targets.size());
  base_ = character.With(weapon_.stat_bonuses).With(WhileShooting(weapon_));
  rolls_ = scenario.seed ^ 0x9E3779B97F4A7C15ULL;
  for (std::size_t i = 0; i < start_.targets.size(); ++i) {
    per_target_[i].hp = start_.targets[i].hp;
  }
  player_speed_ = MovementSpeed(base_.speed);
  for (std::size_t i = 0; i < start_.targets.size(); ++i) {
    wander_to_[i] = start_.targets[i].position;
  }

  for (std::size_t i = 0; i < weapon_.subattacks.size(); ++i) {
    const Subattack& s = weapon_.subattacks[i];
    clocks_.push_back({i, 0, 0, 0});
    Group g{weapon_.FindProjectile(s.projectile_id), {}, static_cast<int>(i), 0, From::kWeapon};
    if (g.projectile) {
      for (int k = 0; k < s.num_projectiles; ++k) {
        g.paths.emplace_back(*g.projectile, s, k);
      }
    }
    groups_.push_back(std::move(g));
  }

  // Procs go off on the shots of the fastest subattack, as the evaluation
  // counts them.
  for (std::size_t i = 0; i < weapon_.subattacks.size(); ++i) {
    if (weapon_.subattacks[i].rate_of_fire > weapon_.subattacks[proc_subattack_].rate_of_fire) {
      proc_subattack_ = i;
    }
  }
  std::vector<int> worn = base_.worn;
  worn.push_back(weapon_.type);
  for (const Activate& act : weapon_.on_shoot) {
    Proc proc{&act, ModelOf(weapon_, act, worn), 0, 0, 0};
    if (proc.model == OnShootModel::kExtraShots) {
      const std::string type = act.Text("type");
      const Projectile& p =
          type.empty() ? weapon_.projectiles[0]
                       : weapon_.created_projectiles.at(static_cast<int>(std::strtol(type.c_str(), nullptr, 0)));
      proc.group = AddGroup(p, act, From::kProc);
    }
    if (proc.model == OnShootModel::kPoisonBurst || proc.model == OnShootModel::kExtraShots ||
        proc.model == OnShootModel::kChanceBoost) {
      procs_.push_back(proc);
    }
  }

  if (!ability_) {
    return;
  }
  // The ability's shots: Shoot fires its subattacks from the player, a
  // spell's burst spreads evenly from the cursor, and created shots fly one
  // at a time, each placed when cast.
  const Ability& a = *ability_;
  activate_group_.assign(a.activates.size(), -1);
  for (std::size_t i = 0; i < a.activates.size(); ++i) {
    const Activate& act = a.activates[i];
    const double bonus = StatModBonus(act, "statModDamage", base_);
    if (act.type == "Shoot" && shoot_groups_.empty()) {
      for (const Subattack& s : a.subattacks) {
        Group g{nullptr, {}, -1, bonus, From::kAbility};
        for (const Projectile& p : a.projectiles) {
          if (p.id == s.projectile_id) {
            g.projectile = &p;
          }
        }
        if (g.projectile) {
          for (int k = 0; k < s.num_projectiles; ++k) {
            g.paths.emplace_back(*g.projectile, s, k);
          }
          shoot_groups_.push_back(groups_.size());
          groups_.push_back(std::move(g));
        }
      }
    } else if ((act.type == "BulletNova" || act.type == "BulletCreate") && !a.projectiles.empty()) {
      const Projectile* p = &a.projectiles[0];
      if (const std::string type = act.Text("type"); !type.empty()) {
        const auto it = a.created_projectiles.find(static_cast<int>(std::strtol(type.c_str(), nullptr, 0)));
        p = it == a.created_projectiles.end() ? nullptr : &it->second;
      }
      if (p != nullptr) {
        activate_group_[i] = static_cast<int>(AddGroup(*p, act, From::kAbility));
      }
    }
  }
  mp_cost_ = a.mp_cost.value_or(0);
  cooldown_ms_ = a.cooldown.value_or(kDefaultCooldownS) * 1000;
  max_mp_ = std::max<double>(base_.max_magic_points, mp_cost_);
  mp_ = start_.full_mp ? max_mp_ : 0;
}

std::size_t Simulation::AddGroup(const Projectile& projectile, const Activate& act, From from) {
  const int count = static_cast<int>(act.Number("numShots").value_or(from == From::kProc ? 1 : 0)) +
                    StatModCount(act, "statModNumShots", base_);
  // A path per shot even when they all go the same way: wavy shots
  // alternate which side they swing to.
  Subattack s;
  s.num_projectiles = std::max(1, count);
  s.arc_gap = act.type == "BulletNova" ? 360.0 / s.num_projectiles : 0;
  Group g{&projectile, {}, -1, StatModBonus(act, "statModDamage", base_), from};
  for (int k = 0; k < s.num_projectiles; ++k) {
    g.paths.emplace_back(projectile, s, k);
  }
  groups_.push_back(std::move(g));
  return groups_.size() - 1;
}

Point Simulation::TargetCenter() const {
  return world_.aim < world_.targets.size() ? world_.targets[world_.aim].position : Point{};
}

double Simulation::Radius() const {
  return world_.aim < world_.targets.size() ? world_.targets[world_.aim].radius : 0;
}

double Simulation::AimAngle() const {
  const Point from = world_.player;
  const Point to = TargetCenter();
  return to.x == from.x && to.y == from.y ? 0 : std::atan2(to.x - from.x, to.y - from.y);
}

Character Simulation::Current() const {
  Character c = base_;
  for (const Timed& b : player_buffs_) {
    if (b.effect == "ATT") {
      c.attack += b.amount;
    } else if (b.effect == "DEX") {
      c.dexterity += b.amount;
    } else {
      c.status.damaging |= b.effect == "Damaging";
      c.status.berserk |= b.effect == "Berserk";
      c.status.weak |= b.effect == "Weak";
      c.status.dazed |= b.effect == "Dazed";
    }
  }
  return c;
}

Target Simulation::TargetNow(std::size_t i) const {
  Target t = world_.targets[i].target;
  for (const Timed& d : target_debuffs_[i]) {
    if (d.from_ms > TimeMs()) {
      continue;
    }
    t.cursed |= d.effect == "Curse";
    t.exposed |= d.effect == "Exposed";
    t.armor_broken |= d.effect == "Armor Broken";
  }
  return t;
}

std::vector<std::string> Simulation::PlayerStatuses() const {
  std::vector<std::string> out;
  for (const Timed& b : player_buffs_) {
    out.push_back(b.amount ? "+" + std::to_string(b.amount) + " " + b.effect : b.effect);
  }
  return out;
}

std::vector<std::string> Simulation::TargetStatuses(std::size_t i) const {
  std::vector<std::string> out;
  for (const Timed& d : target_debuffs_[i]) {
    out.push_back(d.effect);
  }
  return out;
}

double Simulation::HitDamage(const Group& group, Multiplier multiplier, std::size_t target) {
  const Target t = TargetNow(target);
  const Projectile& p = *group.projectile;
  if (start_.random_rolls) {
    // The listed max is never rolled (RealmEye: Weapon Attributes).
    const int roll = Roll(p.min_damage, p.max_damage - 1);
    return group.subattack >= 0
               ? DamageAfterDefense(rotmg::HitDamage(roll, multiplier), EffectiveDefense(t, p.armor_piercing), t.cursed)
               : ExpectedAbilityDamage(roll, roll, group.bonus, t, p.armor_piercing);
  }
  const int flags = (t.armored ? 1 : 0) | (t.armor_broken ? 2 : 0) | (t.exposed ? 4 : 0) | (t.cursed ? 8 : 0) |
                    (group.subattack >= 0 ? 16 : 0);
  const auto key = std::tuple{group.projectile, multiplier.num, multiplier.den, t.defense, flags, group.bonus};
  if (const auto it = damage_cache_.find(key); it != damage_cache_.end()) {
    return it->second;
  }
  // The weapon's damage goes through ATT; an ability's or a proc's doesn't,
  // but takes its StatMod bonus (RealmEye: Character Stats).
  const double damage = group.subattack >= 0
                            ? ExpectedHitDamage(p, multiplier, t)
                            : ExpectedAbilityDamage(p.min_damage, p.max_damage - 1, group.bonus, t, p.armor_piercing);
  damage_cache_.emplace(key, damage);
  return damage;
}

double Simulation::AbilityHit(int low, int high, double bonus, std::size_t target, bool armor_piercing) {
  if (start_.random_rolls) {
    low = high = Roll(low, std::max(low, high));
  }
  return ExpectedAbilityDamage(low, high, bonus, TargetNow(target), armor_piercing);
}

int Simulation::Roll(int low, int high) {
  high = std::max(low, high);
  return std::min(high, low + static_cast<int>(NextRandom(rolls_) * (high - low + 1)));
}

bool Simulation::Invulnerable(std::size_t target) const {
  const SimTarget& t = world_.targets[target];
  if (t.invulnerable_every_s <= 0 || t.invulnerable_s <= 0) {
    return false;
  }
  const double s = TimeMs() / 1000;
  return s >= t.invulnerable_every_s && std::fmod(s, t.invulnerable_every_s) < t.invulnerable_s;
}

bool Simulation::Alive(std::size_t target) const {
  return world_.targets[target].hp <= 0 || per_target_[target].hp > 0;
}

void Simulation::Retarget() {
  if (world_.targets.empty() || Alive(world_.aim)) {
    return;
  }
  double best = 1e18;
  for (std::size_t t = 0; t < world_.targets.size(); ++t) {
    const double d = Distance(world_.player, world_.targets[t].position);
    if (Alive(t) && d < best) {
      best = d;
      world_.aim = t;
    }
  }
}

void Simulation::Advance(double ms) {
  recent_hits_.clear();
  carry_ms_ += ms;
  while (carry_ms_ >= kStepMs) {
    carry_ms_ -= kStepMs;
    Step();
  }
  shots_.clear();
  for (const Live& l : live_) {
    const Group& g = groups_[l.group];
    const ProjectilePath& path = g.paths[static_cast<std::size_t>(l.index)];
    const bool hit = std::ranges::find(l.hit, true) != l.hit.end();
    shots_.push_back({g.subattack, g.from, g.projectile->id, g.projectile->object_id, WorldAt(l, l.step),
                      path.HeadingDegrees(l.step) + l.aim * 180 / std::numbers::pi, l.step * kStepMs, hit});
  }
}

Point Simulation::WorldAt(const Live& shot, int step) const {
  const Point local = groups_[shot.group].paths[static_cast<std::size_t>(shot.index)].At(step);
  // The path's +y is the aim and its +x the aim's right.
  const double s = std::sin(shot.aim);
  const double c = std::cos(shot.aim);
  return {shot.origin.x + local.x * c + local.y * s, shot.origin.y - local.x * s + local.y * c};
}

void Simulation::Step() {
  ++steps_;
  const double now = TimeMs();
  Move();
  Retarget();
  const bool anyone = !world_.targets.empty() && Alive(world_.aim);

  // Buffs and debuffs that have run out go first.
  const auto expired = [now](const Timed& t) { return t.until_ms <= now; };
  std::erase_if(player_buffs_, expired);
  for (auto& list : target_debuffs_) {
    std::erase_if(list, expired);
  }
  std::erase_if(effects_, [now](const Effect& e) { return e.until_ms <= now; });
  // What's up now counts for this whole step; each effect once, however many
  // boosts of it there are.
  std::set<std::string> up;
  for (const Timed& b : player_buffs_) {
    if (b.from_ms <= now && up.insert(b.effect).second) {
      player_uptime_[b.effect] += kStepMs;
    }
  }
  for (std::size_t t = 0; t < target_debuffs_.size(); ++t) {
    up.clear();
    for (const Timed& d : target_debuffs_[t]) {
      if (d.from_ms <= now && up.insert(d.effect).second) {
        target_uptime_[t][d.effect] += kStepMs;
      }
    }
  }

  // Shots already in flight move first, then new volleys spawn, so a shot
  // fired this step is checked at its spawn point and nothing else.
  for (Live& l : live_) {
    ++l.step;
    Land(l);
  }
  // Poison ticks before new poison lands, so each ticks once per step after
  // it lands, and a 4.5 second poison ticks for exactly 4.5 seconds.
  TickPoisons();
  RunPending();
  CheckTraps();

  if (ability_) {
    mp_ = std::min(max_mp_, mp_ + MpPerSecond(Current().wisdom) * kStepMs / 1000);
    if (now >= next_cast_ms_ && mp_ >= mp_cost_ && anyone) {
      mp_ -= mp_cost_;
      next_cast_ms_ = now + cooldown_ms_;
      Cast();
    }
  }

  for (Clock& c : clocks_) {
    const Subattack& s = weapon_.subattacks[c.subattack];
    while (anyone && c.next_ms <= now) {
      // Fire rate follows DEX, Berserk and Dazed as they are at each shot.
      const Character me = Current();
      const double interval = 1000 / (AttacksPerSecond(me.dexterity, me.status) * s.rate_of_fire);
      Fire(c.subattack);
      if (!s.burst_count || !s.burst_delay) {
        c.next_ms += interval;
        continue;
      }
      if (c.burst_fired == 0) {
        c.burst_start_ms = c.next_ms;
      }
      if (++c.burst_fired < *s.burst_count) {
        c.next_ms += interval;
      } else {
        // The delay runs from the start of one burst to the start of the next.
        c.burst_fired = 0;
        c.next_ms =
            std::max(c.next_ms + interval, c.burst_start_ms + BurstDelay(s, me.dexterity, me.status) * 1000);
      }
    }
  }

  std::erase_if(live_, [this](const Live& l) {
    const ProjectilePath& path = groups_[l.group].paths[static_cast<std::size_t>(l.index)];
    if (l.stopped || l.step >= path.Steps()) {
      Retire(l);
      return true;
    }
    return false;
  });

  if (steps_ % static_cast<long>(kHistoryMs / kStepMs) == 0) {
    damage_history_.push_back(bucket_damage_);
    mp_history_.push_back(mp_);
    bucket_damage_ = 0;
  }
}

void Simulation::Spawn(std::size_t group, int index, Point origin, double aim, long volley) {
  const Character me = Current();
  live_.push_back({group, index, volley, 0, origin, aim,
                   WithDamageBonus(DamageMultiplier(me.attack, me.status), weapon_.damage_bonus),
                   std::vector<bool>(world_.targets.size(), false), false});
  Land(live_.back());
}

void Simulation::Fire(std::size_t subattack) {
  const Group& g = groups_[subattack];
  Counters& counters = per_subattack_[subattack];
  ++totals_.volleys;
  ++counters.volleys;
  const long id = next_volley_++;
  open_.push_back({id, static_cast<int>(subattack), static_cast<int>(g.paths.size()), 0});
  const double aim = AimAngle();
  for (int i = 0; i < static_cast<int>(g.paths.size()); ++i) {
    ++totals_.projectiles;
    ++counters.projectiles;
    Spawn(subattack, i, world_.player, aim, id);
  }
  if (subattack == proc_subattack_) {
    OnShoot();
  }
  // A volley with no projectiles is finished as soon as it's fired.
  ResolveFinished();
}

void Simulation::Cast() {
  ++ability_totals_.casts;
  const Ability& a = *ability_;
  const Point cursor = TargetCenter();
  const double aim = AimAngle();
  const double now = TimeMs();
  constexpr double degrees = std::numbers::pi / 180;
  for (std::size_t i = 0; i < a.activates.size(); ++i) {
    const Activate& act = a.activates[i];
    if (const auto buff = BuffOf(act, base_)) {
      if (buff->on == Buff::On::kPlayer) {
        Boost(buff->effect, buff->amount, buff->duration_s, "ability");
      } else {
        // EffectBlast reaches every target within its radius of the cursor.
        const double radius = act.Number("radius").value_or(0);
        for (std::size_t t = 0; t < world_.targets.size(); ++t) {
          if (Alive(t) && Distance(world_.targets[t].position, cursor) <= radius + world_.targets[t].radius) {
            Debuff(t, buff->effect, buff->duration_s);
          }
        }
      }
    }
    if (act.type == "Shoot") {
      for (std::size_t g : shoot_groups_) {
        for (int k = 0; k < static_cast<int>(groups_[g].paths.size()); ++k) {
          Spawn(g, k, world_.player, aim, -1);
        }
      }
    } else if (act.type == "BulletNova" && activate_group_[i] >= 0) {
      // RealmEye, Spells: they target the cursor, and their range is how far
      // the shots go from the centre of the burst.
      const auto g = static_cast<std::size_t>(activate_group_[i]);
      for (int k = 0; k < static_cast<int>(groups_[g].paths.size()); ++k) {
        Spawn(g, k, cursor, aim, -1);
      }
    } else if (act.type == "BulletCreate" && activate_group_[i] >= 0) {
      CreateShots(static_cast<std::size_t>(activate_group_[i]), act);
    } else if (act.type == "DamageNova") {
      // RealmEye, Tomes: an "on-cursor nova" that "explodes 2 times for 0.8
      // seconds". Blasts are spread evenly over `time`, the first at once.
      const int count = static_cast<int>(act.Number("activationCount").value_or(1)) +
                        StatModCount(act, "statModActivationCount", base_);
      const double span = act.Number("time").value_or(0.8) * 1000;
      const double radius = act.Number("radius").value_or(0) + StatModBonus(act, "statModRadius", base_);
      for (int k = 0; k < count; ++k) {
        pending_.push_back({Pending::Kind::kNovaBlast, now + (count > 1 ? span * k / (count - 1) : 0), cursor, radius,
                            &act, From::kAbility});
      }
    } else if (act.type == "Lightning") {
      // RealmEye, Scepters: the closest enemy in a 30 degree cone towards the
      // cursor starts the chain, which jumps to an enemy not yet hit within
      // about 8 tiles of the last, losing damage each time. Scepters listed
      // with another cone or a range have them as arcDegrees and range.
      const int count = LightningTargets(act, base_);
      const double damage = LightningDamage(act, base_);
      const double drop = act.Number("decrDamage").value_or(0);
      const double half_cone = act.Number("arcDegrees").value_or(30) / 2 * degrees;
      const double reach = act.Number("range").value_or(1e9);
      std::vector<bool> struck(world_.targets.size(), false);
      std::optional<std::size_t> at;
      double best = 1e18;
      for (std::size_t t = 0; t < world_.targets.size(); ++t) {
        if (!Alive(t)) {
          continue;
        }
        const Point p = world_.targets[t].position;
        const double angle = std::atan2(p.x - world_.player.x, p.y - world_.player.y);
        const double off = std::abs(std::remainder(angle - aim, 2 * std::numbers::pi));
        const double d = Distance(world_.player, p);
        if (off <= half_cone && d <= reach && d < best) {
          best = d;
          at = t;
        }
      }
      Effect bolt{Effect::Kind::kBolt, world_.player, 0, now + 150, {world_.player}, false};
      for (int k = 0; k < count && at; ++k) {
        struck[*at] = true;
        ++ability_totals_.hits;
        Damage(*at, AbilityHit(0, 0, std::max(0.0, damage - drop * k), *at, false), From::kAbility);
        bolt.chain.push_back(world_.targets[*at].position);
        std::optional<std::size_t> next;
        double nearest = 8;
        for (std::size_t t = 0; t < world_.targets.size(); ++t) {
          const double d = Distance(world_.targets[*at].position, world_.targets[t].position);
          if (!struck[t] && Alive(t) && d <= nearest) {
            nearest = d;
            next = t;
          }
        }
        at = next;
      }
      effects_.push_back(std::move(bolt));
    } else if (act.type == "PoisonGrenade") {
      // RealmEye, Poisons: thrown at the cursor, it lands 0.8 seconds later
      // unless the item says.
      const double radius = act.Number("radius").value_or(0) + StatModBonus(act, "statModRadius", base_);
      pending_.push_back({Pending::Kind::kPoisonLands, now + act.Number("throwTime").value_or(0.8) * 1000, cursor,
                          radius, &act, From::kAbility});
    } else if (act.type == "Trap") {
      // RealmEye, Traps: thrown at the cursor in 1 second, it lasts 10
      // seconds, goes off when an enemy comes within half its explosion
      // radius, at once if it lands on one, and at the end if nothing does.
      const double radius = act.Number("radius").value_or(0) + StatModBonus(act, "statModTrapRadius", base_);
      const double lands = now + act.Number("throwTime").value_or(1) * 1000;
      traps_.push_back({cursor, lands, lands + act.Number("armTime").value_or(0) * 1000,
                        lands + act.Number("duration").value_or(10) * 1000, radius,
                        radius * act.Number("sensitivity").value_or(0.5), &act});
    }
  }
}

void Simulation::CreateShots(std::size_t group, const Activate& act) {
  // RealmEye, Wakizashi: a slash across the line of aim "centered on the
  // crosshair", cast at most 4.4 tiles away unless the item says. Each shot
  // is placed so it crosses its spot halfway through its flight; that's an
  // assumption, which items with minDistance and maxDistance both half their
  // shots' range bear out: their shots then start at the player.
  const Point cursor = TargetCenter();
  const double aim = AimAngle();
  constexpr double degrees = std::numbers::pi / 180;
  const double reach = act.Number("maxDistance").value_or(4.4);
  const double closest = act.Number("minDistance").value_or(0);
  const double to_cursor = std::clamp(Distance(world_.player, cursor), closest, std::max(closest, reach));
  const Point centre = Add(world_.player, {std::sin(aim), std::cos(aim)}, to_cursor);
  // The evaluation counts a proc with no numShots as one shot, and an
  // ability's as none.
  const int count = static_cast<int>(act.Number("numShots").value_or(groups_[group].from == From::kProc ? 1 : 0)) +
                    StatModCount(act, "statModNumShots", base_);
  const double heading = aim + act.Number("offsetAngle").value_or(0) * degrees;
  const double spread = heading + act.Number("gapAngle").value_or(0) * degrees;
  for (int k = 0; k < count; ++k) {
    const double from_middle = k - (count - 1) / 2.0;
    const double shot = heading + act.Number("arcGap").value_or(0) * degrees * from_middle;
    const Point spot =
        Add(centre, {std::sin(spread), std::cos(spread)}, act.Number("gapTiles").value_or(0) * from_middle);
    const int index = k % static_cast<int>(groups_[group].paths.size());
    const ProjectilePath& path = groups_[group].paths[static_cast<std::size_t>(index)];
    const Point half = path.At(path.Steps() / 2);
    const double sn = std::sin(shot);
    const double cs = std::cos(shot);
    Spawn(group, index, {spot.x - half.x * cs - half.y * sn, spot.y + half.x * sn - half.y * cs}, shot, -1);
  }
}

void Simulation::Boost(const std::string& effect, int amount, double duration_s, const std::string& channel) {
  const double now = TimeMs();
  const double until = now + duration_s * 1000;
  const auto same = std::ranges::find_if(
      player_buffs_, [&](const Timed& t) { return t.effect == effect && t.channel == channel; });
  if (same == player_buffs_.end()) {
    player_buffs_.push_back({effect, amount, until, now, channel});
  } else {
    same->amount = std::max(same->amount, amount);
    same->until_ms = std::max(same->until_ms, until);
  }
}

void Simulation::OnShoot() {
  const double now = TimeMs();
  for (std::size_t i = 0; i < procs_.size(); ++i) {
    Proc& proc = procs_[i];
    const Activate& act = *proc.effect;
    if (now < proc.ready_ms) {
      continue;
    }
    if (proc.model == OnShootModel::kChanceBoost) {
      // Each shot adds its chance; the boost goes off when they add up to
      // one, so it comes as often as the chance says on average. With random
      // rolls each shot rolls instead.
      const double chance = act.Number("proc").value_or(1);
      if (start_.random_rolls) {
        if (NextRandom(rolls_) >= chance) {
          continue;
        }
      } else {
        proc.chance += chance;
        if (proc.chance < 1) {
          continue;
        }
        proc.chance = 0;
      }
      // Boosts of one stat on a noStack channel don't stack (RealmEye:
      // Enchantments); any others stack.
      const std::string channel = act.Text("channel");
      Boost(act.Text("stat"), static_cast<int>(act.Number("amount").value_or(0)), act.Number("duration").value_or(0),
            channel.starts_with("noStack") ? channel : "proc " + std::to_string(i));
    } else if (proc.model == OnShootModel::kPoisonBurst) {
      // Nothing aims it, so it bursts around the player.
      pending_.push_back({Pending::Kind::kPoisonLands, now + act.Number("throwTime").value_or(0) * 1000,
                          world_.player, act.Number("radius").value_or(0), &act, From::kProc});
    } else {
      CreateShots(proc.group, act);
    }
    ++proc_totals_.casts;
    proc.ready_ms = now + act.Number("cooldown").value_or(0) * 1000;
  }
}

void Simulation::RunPending() {
  const double now = TimeMs();
  std::vector<Pending> due;
  std::erase_if(pending_, [&](const Pending& p) {
    if (p.at_ms > now) {
      return false;
    }
    due.push_back(p);
    return true;
  });
  for (const Pending& p : due) {
    const Activate& act = *p.activate;
    const auto inside = [&](std::size_t t) {
      return Alive(t) && Distance(world_.targets[t].position, p.centre) <= p.radius + world_.targets[t].radius;
    };
    if (p.kind == Pending::Kind::kNovaBlast) {
      const double bonus = StatModBonus(act, "statModDamage", base_);
      for (std::size_t t = 0; t < world_.targets.size(); ++t) {
        if (inside(t)) {
          Damage(t,
                 AbilityHit(static_cast<int>(act.Number("minDamage").value_or(0)),
                            static_cast<int>(act.Number("maxDamage").value_or(0)), bonus, t, false),
                 p.from);
        }
      }
      effects_.push_back({Effect::Kind::kBlast, p.centre, p.radius, now + 200, {}, false});
    } else {
      // Poison ignores DEF and every poison stacks (RealmEye: Poisons). The
      // impact lands at once and the rest evenly over the duration.
      const double impact = act.Number("impactDamage").value_or(0) + StatModBonus(act, "statModImpactDamage", base_);
      const double total = act.Number("totalDamage").value_or(0) + StatModBonus(act, "statModDamage", base_);
      // With no duration it all comes on the next step.
      const double duration_ms = std::max(kStepMs, act.Number("duration").value_or(0) * 1000);
      Poison poison{{}, total * kStepMs / duration_ms, total, p.from};
      for (std::size_t t = 0; t < world_.targets.size(); ++t) {
        if (!inside(t)) {
          continue;
        }
        poison.targets.push_back(t);
        Damage(t, impact * (TargetNow(t).cursed ? 1.25 : 1), p.from);
        if (const std::string effect = act.Text("condEffect"); !effect.empty()) {
          Debuff(t, effect, act.Number("condDuration").value_or(0));
        }
      }
      poisons_.push_back(std::move(poison));
      effects_.push_back({Effect::Kind::kPoison, p.centre, p.radius, now + duration_ms, {}, false});
    }
  }
}

void Simulation::TickPoisons() {
  for (Poison& p : poisons_) {
    const double tick = std::min(p.per_step, p.left);
    p.left -= tick;
    for (std::size_t t : p.targets) {
      Damage(t, tick * (TargetNow(t).cursed ? 1.25 : 1), p.from, false);
    }
  }
  std::erase_if(poisons_, [](const Poison& p) { return p.left <= 1e-9; });
}

void Simulation::CheckTraps() {
  const double now = TimeMs();
  std::vector<Trap> boom;
  std::erase_if(traps_, [&](const Trap& trap) {
    if (now < trap.lands_ms) {
      return false;
    }
    const bool landing = now - kStepMs < trap.lands_ms;
    bool near = false;
    for (std::size_t t = 0; t < world_.targets.size(); ++t) {
      const SimTarget& target = world_.targets[t];
      near |= Alive(t) && Distance(target.position, trap.at) <= trap.trigger + target.radius;
    }
    if ((near && (landing || now >= trap.armed_ms)) || now >= trap.expires_ms) {
      boom.push_back(trap);
      return true;
    }
    return false;
  });
  for (const Trap& trap : boom) {
    Explode(trap);
  }
  effects_.erase(std::remove_if(effects_.begin(), effects_.end(),
                                [](const Effect& e) { return e.kind == Effect::Kind::kTrap; }),
                 effects_.end());
  for (const Trap& trap : traps_) {
    if (now >= trap.lands_ms) {
      effects_.push_back({Effect::Kind::kTrap, trap.at, trap.trigger, trap.expires_ms, {}, now >= trap.armed_ms});
    }
  }
}

void Simulation::Explode(const Trap& trap) {
  const Activate& act = *trap.activate;
  const int damage = static_cast<int>(act.Number("totalDamage").value_or(0));
  const double bonus = StatModBonus(act, "statModTrapDamage", base_);
  const std::string effect = act.Text("condEffect");
  for (std::size_t t = 0; t < world_.targets.size(); ++t) {
    if (Alive(t) && Distance(world_.targets[t].position, trap.at) <= trap.radius + world_.targets[t].radius) {
      Damage(t, AbilityHit(damage, damage, bonus, t, act.Flag("armorPierce")), From::kAbility);
      if (!effect.empty()) {
        Debuff(t, effect, act.Number("condDuration").value_or(0) + StatModBonus(act, "statModTrapCondDuration", base_));
      }
    }
  }
  effects_.push_back({Effect::Kind::kBlast, trap.at, trap.radius, TimeMs() + 200, {}, false});
}

void Simulation::Debuff(std::size_t target, const std::string& raw, double duration_s, double delay_ms) {
  const std::string effect = raw == "ArmorBroken" ? "Armor Broken" : raw;
  // Only the statuses that change damage matter here.
  if (effect != "Curse" && effect != "Exposed" && effect != "Armor Broken") {
    return;
  }
  const double from = TimeMs() + delay_ms;
  const double until = from + duration_s * 1000;
  auto& list = target_debuffs_[target];
  const auto same = std::ranges::find(list, effect, &Timed::effect);
  if (same == list.end()) {
    list.push_back({effect, 0, until, from, ""});
  } else {
    same->until_ms = std::max(same->until_ms, until);
  }
}

double Simulation::Damage(std::size_t target, double amount, From from, bool shown) {
  TargetCounters& c = per_target_[target];
  // Shots still hit an invulnerable target and stop on it; they just do
  // nothing. That's an assumption.
  if (Invulnerable(target)) {
    return 0;
  }
  const double max_hp = world_.targets[target].hp;
  if (max_hp > 0) {
    if (c.hp <= 0) {
      return 0;
    }
    amount = std::min(amount, c.hp);
    c.hp -= amount;
  }
  c.damage += amount;
  bucket_damage_ += amount;
  if (from == From::kAbility) {
    c.ability_damage += amount;
    ability_totals_.damage += amount;
  } else if (from == From::kProc) {
    proc_totals_.damage += amount;
  }
  if (shown && recent_hits_.size() < 256) {
    recent_hits_.push_back({target, amount, from});
  }
  if (max_hp > 0 && c.hp <= 0) {
    ++c.kills;
    if (c.killed_ms < 0) {
      c.killed_ms = TimeMs();
    }
    if (start_.respawn) {
      c.hp = max_hp;
      target_debuffs_[target].clear();
    }
  }
  return amount;
}

void Simulation::Land(Live& shot) {
  if (shot.stopped) {
    return;
  }
  const Group& g = groups_[shot.group];
  const ProjectilePath& path = g.paths[static_cast<std::size_t>(shot.index)];
  if (path.Steps() == 0) {
    return;
  }
  const Point here = WorldAt(shot, shot.step);
  const Point before = shot.step == 0 ? here : WorldAt(shot, shot.step - 1);

  // A shot that doesn't pierce stops at the first target it reaches this
  // step; one that pierces hits every target it passes through.
  std::vector<std::pair<double, std::size_t>> touched;
  for (std::size_t i = 0; i < world_.targets.size(); ++i) {
    if (shot.hit[i] || !Alive(i)) {
      continue;
    }
    const SimTarget& t = world_.targets[i];
    if (const auto entry = Entry(before, here, t.position, t.radius)) {
      touched.push_back({*entry, i});
    }
  }
  if (touched.empty()) {
    return;
  }
  std::ranges::sort(touched);
  if (!g.projectile->multi_hit) {
    touched.resize(1);
    shot.stopped = true;
  }

  for (const auto& [entry, i] : touched) {
    shot.hit[i] = true;
    ++per_target_[i].hits;
    const double damage = Damage(i, HitDamage(g, shot.multiplier, i), g.from);
    if (g.subattack < 0) {
      ++(g.from == From::kProc ? proc_totals_ : ability_totals_).hits;
    } else {
      Counters& counters = per_subattack_[static_cast<std::size_t>(g.subattack)];
      ++totals_.hits;
      ++counters.hits;
      totals_.damage += damage;
      counters.damage += damage;
      for (OpenVolley& v : open_) {
        if (v.id == shot.volley) {
          ++v.hits;
          break;
        }
      }
    }
    // The shot's own status effects land on what it hits from the next step,
    // so shots of the same volley landing together don't gain from them. The
    // server applies them, so in the game they come later still.
    for (const ConditionEffect& e : g.projectile->condition_effects) {
      Debuff(i, e.effect, e.duration, kStepMs);
    }
  }
}

void Simulation::Retire(const Live& shot) {
  if (shot.volley < 0) {
    return;
  }
  for (OpenVolley& v : open_) {
    if (v.id == shot.volley) {
      --v.remaining;
      break;
    }
  }
  ResolveFinished();
}

void Simulation::ResolveFinished() {
  // Volleys resolve in the order they were fired, so the counters only ever
  // include volleys that are completely finished.
  while (!open_.empty() && open_.front().remaining == 0) {
    const OpenVolley& v = open_.front();
    Counters& counters = per_subattack_[static_cast<std::size_t>(v.subattack)];
    ++totals_.resolved_volleys;
    ++counters.resolved_volleys;
    totals_.resolved_hits += v.hits;
    counters.resolved_hits += v.hits;
    open_.pop_front();
  }
}

}  // namespace rotmg
