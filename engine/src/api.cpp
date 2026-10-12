#include "rotmg/api.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <memory>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "rotmg/build.hpp"
#include "rotmg/dataset.hpp"
#include "rotmg/evaluate.hpp"
#include "rotmg/enchant.hpp"
#include "rotmg/hits.hpp"
#include "rotmg/sets.hpp"

// Request:
//   {"weapon": "Doom Bow", "ability": "Magic Quiver",
//    "attack": 50, "dexterity": 50, "wisdom": 50, "vitality": 40,
//    "defense": 25, "speed": 50, "maxHitPoints": 700, "maxMagicPoints": 300,
//    "status": {"weak", "damaging", "berserk", "dazed"},
//    "target": {"defense", "armored", "armorBroken", "exposed", "cursed"},
//    "wearing": [14381, 14382], "enchantments": ["Damage_Bonus_4"], "class": "Archer",
//    "hitsPerVolley": [null, 1.5], "curatedHitsPerVolley": [2, null],
//    "hitModel": {"distance": 4, "radius": 0.5},
//    "curve": {"from": 0, "to": 150, "step": 5},
//    "hitCurve": {"from": 0.5, "to": 10, "step": 0.25}}
// Only "weapon" is required. "wearing" lists the item types of other gear
// worn, like a set's armor or a ring a weapon's procs need.
// "enchantments" are ids of up to 4 enchantments on the weapon, and
// "gearEnchantments" up to 4 each on {"ring", "armor", "ability"}; relative
// ones ("% of Bonus ATT") need "class", the class id. "curve" adds
// the build's DPS at each DEF in the range, with everything else in the
// request unchanged. "hitCurve" adds the geometric estimate's total hits
// per volley at each distance.
//
// Response:
//   {"ok": true, "breakdown": {...the weapon on its own...},
//    "build": {"dps", "weaponDps", "uptimes": [...], "assumptions": [...]},
//    "ability": {...}, "sets": [{"type", "name", "pieces", "stats", "unsupported"}],
//    "enchantmentStats": [{"stat", "amount"}],
//    "curve": [{"defense", "dps"}],
//    "hitCurve": [{"distance", "hits"}], "hitModelGaps": ["..."]}
// "ability" is only there when the request names one.
//   {"ok": false, "error": "..."}
namespace rotmg {
namespace {

using nlohmann::json;

const char* SourceName(Source source) {
  switch (source) {
    case Source::kWiki:
      return "wiki";
    case Source::kClientData:
      return "client-data";
    case Source::kGeometricEstimate:
      return "geometric-estimate";
    case Source::kCurated:
      return "curated";
    case Source::kUser:
      return "user";
    case Source::kAssumed:
      return "assumed";
  }
  return "assumed";
}

json ToJson(const Breakdown& b) {
  json subattacks = json::array();
  for (const SubattackResult& s : b.subattacks) {
    subattacks.push_back({{"projectileId", s.projectile_id},
                          {"projectilesPerVolley", s.projectiles_per_volley},
                          {"volleysPerSecond", s.volleys_per_second},
                          {"hitsPerVolley", s.hits_per_volley},
                          {"hitsSource", SourceName(s.hits_source)},
                          {"damagePerHit", s.damage_per_hit},
                          {"dps", s.dps}});
  }
  json procs = json::array();
  for (const ProcResult& p : b.procs) {
    procs.push_back({{"name", p.name},
                     {"perSecond", p.per_second},
                     {"damage", p.damage},
                     {"dps", p.dps},
                     {"reference", p.reference}});
  }
  json terms = json::array();
  for (const Term& t : b.terms) {
    terms.push_back({{"name", t.name}, {"value", t.value}, {"source", SourceName(t.source)}, {"reference", t.reference}});
  }
  return {{"dps", b.dps},
          {"subattacks", subattacks},
          {"procs", procs},
          {"terms", terms},
          {"assumptions", b.assumptions},
          {"unsupported", b.unsupported}};
}

json ToJson(const Buff& b) {
  return {{"effect", b.effect},
          {"on", b.on == Buff::On::kPlayer ? "player" : "target"},
          {"amount", b.amount},
          {"durationS", b.duration_s}};
}

json ToJson(const AbilityBreakdown& a) {
  json parts = json::array();
  for (const AbilityPart& p : a.parts) {
    parts.push_back({{"name", p.name}, {"damage", p.damage}, {"reference", p.reference}});
  }
  json buffs = json::array();
  for (const Buff& b : a.buffs) {
    buffs.push_back(ToJson(b));
  }
  json terms = json::array();
  for (const Term& t : a.terms) {
    terms.push_back({{"name", t.name}, {"value", t.value}, {"source", SourceName(t.source)}, {"reference", t.reference}});
  }
  return {{"mpCost", a.mp_cost},
          {"cooldownS", a.cooldown_s},
          {"mpPerSecond", a.mp_per_second},
          {"castsPerSecond", a.casts_per_second},
          {"damagePerCast", a.damage_per_cast},
          {"dps", a.dps},
          {"parts", parts},
          {"buffs", buffs},
          {"terms", terms},
          {"assumptions", a.assumptions},
          {"unsupported", a.unsupported}};
}

json ToJson(const BuildBreakdown& b) {
  json uptimes = json::array();
  for (const Uptime& u : b.uptimes) {
    json entry = ToJson(u.buff);
    entry["share"] = u.share;
    uptimes.push_back(entry);
  }
  return {{"dps", b.dps}, {"weaponDps", b.weapon_dps}, {"uptimes", uptimes}, {"assumptions", b.assumptions}};
}

json Error(const std::string& message) {
  return {{"ok", false}, {"error", message}};
}

struct Request {
  const Weapon* weapon = nullptr;
  const Ability* ability = nullptr;
  bool unknown_ability = false;
  Character character;
  Target target;
  Assumptions assumptions;
  std::vector<WornSet> sets;
  // The weapon with its enchantments, when it has any; `weapon` points here.
  std::shared_ptr<const Weapon> enchanted;
  // The ability with its enchantments, when they change it; `ability` points here.
  std::shared_ptr<const Ability> enchanted_ability;
  std::vector<StatBonus> enchantment_stats;
  // Assumptions the request's gear brings, for the build's list.
  std::vector<std::string> notes;
};

// Reads the parts of a request Evaluate and StartSimulation share. Throws on
// malformed JSON; an unknown weapon leaves `weapon` null.
Request ParseRequest(const json& req, const Dataset& dataset) {
  Request r;
  r.weapon = FindWeapon(dataset, req.at("weapon").get<std::string>());
  if (auto it = req.find("ability"); it != req.end() && !it->is_null()) {
    r.ability = FindAbility(dataset, it->get<std::string>());
    r.unknown_ability = r.ability == nullptr;
  }
  r.character.attack = req.value("attack", 0);
  r.character.dexterity = req.value("dexterity", 0);
  r.character.wisdom = req.value("wisdom", 0);
  r.character.vitality = req.value("vitality", 0);
  r.character.defense = req.value("defense", 0);
  r.character.speed = req.value("speed", 0);
  r.character.max_hit_points = req.value("maxHitPoints", 0);
  r.character.max_magic_points = req.value("maxMagicPoints", 0);
  if (auto it = req.find("status"); it != req.end()) {
    r.character.status.weak = it->value("weak", false);
    r.character.status.damaging = it->value("damaging", false);
    r.character.status.berserk = it->value("berserk", false);
    r.character.status.dazed = it->value("dazed", false);
  }
  if (auto it = req.find("target"); it != req.end()) {
    r.target.defense = it->value("defense", 0);
    r.target.armored = it->value("armored", false);
    r.target.armor_broken = it->value("armorBroken", false);
    r.target.exposed = it->value("exposed", false);
    r.target.cursed = it->value("cursed", false);
  }
  const auto read_hits = [&req](const char* key, std::vector<std::optional<double>>& out) {
    if (auto it = req.find(key); it != req.end()) {
      for (const json& h : *it) {
        out.push_back(h.is_null() ? std::nullopt : std::optional<double>(h.get<double>()));
      }
    }
  };
  read_hits("hitsPerVolley", r.assumptions.hits_per_volley);
  read_hits("curatedHitsPerVolley", r.assumptions.curated_hits_per_volley);
  if (auto it = req.find("hitModel"); it != req.end()) {
    r.assumptions.distance = it->value("distance", 4.0);
    r.assumptions.target_radius = it->value("radius", kDefaultTargetRadius);
  }
  // Set bonuses count the weapon and ability plus the other pieces worn,
  // and add to the character like gear does.
  std::vector<int> worn = req.value("wearing", std::vector<int>{});
  if (r.weapon) {
    worn.push_back(r.weapon->type);
  }
  if (r.ability) {
    worn.push_back(r.ability->type);
  }
  r.sets = WornSets(dataset, worn);
  for (const WornSet& s : r.sets) {
    r.character = r.character.With(s.stats);
  }
  r.character.worn = worn;
  // Reads one item's enchantment ids, checking each fits and that none clash.
  const auto pick = [&](const json& ids, const std::string& item, const auto& fits) {
    std::vector<const Enchantment*> picked;
    for (const json& id : ids) {
      const auto e = std::ranges::find(dataset.enchantments, id.get<std::string>(), &Enchantment::id);
      if (e == dataset.enchantments.end()) {
        throw std::runtime_error("no enchantment named \"" + id.get<std::string>() + "\"");
      }
      if (!fits(*e)) {
        throw std::runtime_error(e->name + " doesn't go on " + item);
      }
      for (const Enchantment* other : picked) {
        if (Conflict(*e, *other)) {
          throw std::runtime_error(e->name + " and " + other->name + " can't share an item");
        }
      }
      picked.push_back(&*e);
    }
    // Divine items have 4 slots, the most (RealmEye: Enchanting).
    if (picked.size() > 4) {
      throw std::runtime_error("an item holds at most 4 enchantments");
    }
    return picked;
  };
  const auto on_weapon = req.find("enchantments");
  const auto on_gear = req.find("gearEnchantments");
  if ((on_weapon != req.end() || on_gear != req.end()) && r.weapon != nullptr) {
    const std::vector<const Enchantment*> picked =
        on_weapon == req.end() ? std::vector<const Enchantment*>{}
                               : pick(*on_weapon, r.weapon->name, [&](const Enchantment& e) { return Fits(e, *r.weapon); });
    GearEnchantments gear;
    if (on_gear != req.end()) {
      for (const auto& [key, label] : {std::pair{"ring", "RING"}, {"armor", "ARMOR"}, {"ability", "ABILITY"}}) {
        if (auto ids = on_gear->find(key); ids != on_gear->end()) {
          gear.emplace_back(label, pick(*ids, std::string("your ") + key,
                                        [&](const Enchantment& e) { return FitsSlot(e, label); }));
        }
      }
    }
    Enchanted en = Enchant(*r.weapon, picked, gear);
    if (en.mp_cost != 1 && r.ability != nullptr && r.ability->mp_cost) {
      Ability cheaper = *r.ability;
      cheaper.mp_cost = *cheaper.mp_cost * en.mp_cost;
      r.enchanted_ability = std::make_shared<const Ability>(std::move(cheaper));
      r.ability = r.enchanted_ability.get();
    }
    r.enchantment_stats = en.stats;
    r.character = r.character.With(en.stats);
    if (!en.stats.empty()) {
      r.notes.emplace_back("fractional stat bonuses from enchantments add up and the total rounds down");
    }
    const auto class_id = req.find("class");
    const auto cls = std::ranges::find(
        dataset.classes, class_id != req.end() && class_id->is_string() ? class_id->get<std::string>() : "", &Class::id);
    if (!en.relative.empty() && cls == dataset.classes.end()) {
      en.unsupported.emplace_back("relative stat bonuses, which need the class");
    } else if (!en.relative.empty()) {
      Character everything = r.character.With(r.weapon->stat_bonuses);
      if (r.ability != nullptr) {
        everything = everything.With(r.ability->stat_bonuses);
      }
      const std::vector<StatBonus> gains = RelativeBonuses(en.relative, *cls, everything);
      r.character = r.character.With(gains);
      for (const StatBonus& g : gains) {
        const auto same = std::ranges::find(r.enchantment_stats, g.stat, &StatBonus::stat);
        if (same == r.enchantment_stats.end()) {
          r.enchantment_stats.push_back(g);
        } else {
          same->amount += g.amount;
        }
      }
      r.notes.emplace_back(
          "relative enchantments count stats above the class's max as bonus stats, as on a maxed character, and "
          "round down");
    }
    en.weapon.unsupported.insert(en.weapon.unsupported.end(), en.unsupported.begin(), en.unsupported.end());
    r.enchanted = std::make_shared<const Weapon>(std::move(en.weapon));
    r.weapon = r.enchanted.get();
  }
  return r;
}

json ToJson(const WornSet& s) {
  json stats = json::array();
  for (const StatBonus& b : s.stats) {
    stats.push_back({{"stat", b.stat}, {"amount", b.amount}});
  }
  return {{"type", s.set->type},
          {"name", s.set->name},
          {"pieces", s.pieces},
          {"stats", stats},
          {"unsupported", s.set->unsupported}};
}

json ToJson(Point p) {
  return {{"x", p.x}, {"y", p.y}};
}

Movement ReadMovement(const json& j) {
  static const std::map<std::string, Movement::Kind, std::less<>> kinds = {
      {"still", Movement::Kind::kStill},
      {"strafe", Movement::Kind::kStrafe},
      {"circle", Movement::Kind::kCircle},
      {"chargeRetreat", Movement::Kind::kChargeRetreat},
      {"wander", Movement::Kind::kWander},
      {"keepDistance", Movement::Kind::kKeepDistance},
      {"orbit", Movement::Kind::kOrbit},
      {"kite", Movement::Kind::kKite},
  };
  const std::string kind = j.value("kind", std::string("still"));
  const auto it = kinds.find(kind);
  if (it == kinds.end()) {
    throw std::runtime_error("unknown movement \"" + kind + "\"");
  }
  return {it->second, j.value("speed", 0.0), j.value("range", 0.0), j.value("far", 0.0), j.value("clockwise", true)};
}

// The simulation's scene. Without "scenario" it's one target straight ahead
// at the hit model's distance. Targets take the request's target statuses
// and DEF unless they set their own.
Scenario ReadScenario(const json& req, const Request& r) {
  const double radius = r.assumptions.target_radius;
  auto it = req.find("scenario");
  if (it == req.end()) {
    return {{0, 0}, {{{0, r.assumptions.distance.value_or(4.0)}, radius, r.target}}, 0};
  }
  Scenario s;
  s.full_mp = it->value("fullMp", true);
  s.respawn = it->value("respawn", false);
  s.random_rolls = it->value("randomRolls", false);
  if (auto p = it->find("player"); p != it->end()) {
    s.player = {p->value("x", 0.0), p->value("y", 0.0)};
    if (auto m = p->find("movement"); m != p->end()) {
      s.player_movement = ReadMovement(*m);
    }
  }
  s.seed = it->value("seed", 1ULL);
  for (const json& t : it->at("targets")) {
    SimTarget target{{t.at("x").get<double>(), t.at("y").get<double>()}, t.value("radius", radius), r.target};
    target.target.defense = t.value("defense", r.target.defense);
    target.target.armored = t.value("armored", r.target.armored);
    target.target.armor_broken = t.value("armorBroken", r.target.armor_broken);
    target.target.exposed = t.value("exposed", r.target.exposed);
    target.target.cursed = t.value("cursed", r.target.cursed);
    target.hp = t.value("hp", 0.0);
    if (auto m = t.find("movement"); m != t.end()) {
      target.movement = ReadMovement(*m);
    }
    s.targets.push_back(target);
  }
  if (s.targets.empty()) {
    throw std::runtime_error("a scenario needs at least one target");
  }
  s.aim = std::min(it->value("aim", std::size_t{0}), s.targets.size() - 1);
  return s;
}

json ToJson(const Simulation::Counters& c) {
  return {{"volleys", c.volleys},
          {"projectiles", c.projectiles},
          {"hits", c.hits},
          {"damage", c.damage},
          {"resolvedVolleys", c.resolved_volleys},
          {"resolvedHits", c.resolved_hits}};
}

}  // namespace

std::string Api::Load(std::string_view dataset_json) {
  try {
    dataset_ = ParseDataset(dataset_json);
    return "";
  } catch (const DatasetError& e) {
    dataset_.reset();
    return e.what();
  }
}

std::string Api::Evaluate(std::string_view request_json) const {
  if (!dataset_) {
    return Error("no dataset loaded").dump();
  }
  try {
    const json req = json::parse(request_json);
    const Request r = ParseRequest(req, *dataset_);
    const auto& [weapon, ability, unknown_ability, character, target, assumptions, sets, enchanted, enchanted_ability,
                 enchantment_stats, notes] = r;
    if (weapon == nullptr) {
      return Error("no weapon named \"" + req.at("weapon").get<std::string>() + "\"").dump();
    }
    if (unknown_ability) {
      return Error("no ability named \"" + req.at("ability").get<std::string>() + "\"").dump();
    }

    const BuildBreakdown build = EvaluateBuild(*weapon, ability, character, target, assumptions);
    json res = {{"ok", true}, {"breakdown", ToJson(build.weapon)}, {"build", ToJson(build)}};
    if (build.ability) {
      res["ability"] = ToJson(*build.ability);
    }
    res["sets"] = json::array();
    for (const WornSet& s : sets) {
      res["sets"].push_back(ToJson(s));
    }
    res["enchantmentStats"] = json::array();
    for (const StatBonus& b : enchantment_stats) {
      res["enchantmentStats"].push_back({{"stat", b.stat}, {"amount", b.amount}});
    }
    for (const std::string& note : notes) {
      res["build"]["assumptions"].push_back(note);
    }
    if (auto it = req.find("curve"); it != req.end()) {
      const int from = it->value("from", 0);
      const int to = it->value("to", 150);
      const int step = it->value("step", 5);
      if (step <= 0 || to < from || (to - from) / step > 10000) {
        return Error("bad curve range").dump();
      }
      json curve = json::array();
      for (int def = from; def <= to; def += step) {
        Target t = target;
        t.defense = def;
        curve.push_back({{"defense", def}, {"dps", EvaluateBuild(*weapon, ability, character, t, assumptions).dps}});
      }
      res["curve"] = curve;
    }
    if (auto it = req.find("hitCurve"); it != req.end()) {
      const double from = it->value("from", 0.5);
      const double to = it->value("to", 10.0);
      const double step = it->value("step", 0.25);
      if (step <= 0 || to < from || (to - from) / step > 10000) {
        return Error("bad hit curve range").dump();
      }
      json points = json::array();
      for (int k = 0; from + k * step <= to + 1e-9; ++k) {
        const double distance = from + k * step;
        int hits = 0;
        for (const Subattack& s : weapon->subattacks) {
          hits += HitsPerVolley(*weapon, s, distance, assumptions.target_radius);
        }
        points.push_back({{"distance", distance}, {"hits", hits}});
      }
      res["hitCurve"] = points;
      res["hitModelGaps"] = HitModelGaps(*weapon);
    }
    return res.dump();
  } catch (const std::exception& e) {
    return Error(std::string("bad request: ") + e.what()).dump();
  }
}

std::string Api::StartSimulation(std::string_view request_json) {
  simulation_.reset();
  history_sent_ = 0;
  if (!dataset_) {
    return "no dataset loaded";
  }
  try {
    const json req = json::parse(request_json);
    const Request r = ParseRequest(req, *dataset_);
    if (r.weapon == nullptr) {
      return "no weapon named \"" + req.at("weapon").get<std::string>() + "\"";
    }
    const Character character = r.ability ? r.character.With(r.ability->stat_bonuses) : r.character;
    simulation_ = std::make_unique<Simulation>(*r.weapon, character, ReadScenario(req, r), r.ability);
    return "";
  } catch (const std::exception& e) {
    return std::string("bad request: ") + e.what();
  }
}

// A simulation request is an evaluate request plus, optionally,
//   "scenario": {"player": {"x", "y", "movement"}, "aim": 0, "seed": 1,
//                "fullMp": true, "respawn": false, "randomRolls": false,
//                "targets": [{"x", "y", "radius", "defense", "armored", ...,
//                             "movement", "hp"}]}
// in tiles, with +y the way the player faces at the start. A movement is
// {"kind", "speed", "range", "far", "clockwise"}, kind one of still, strafe,
// circle, chargeRetreat, wander (targets), keepDistance, orbit, kite and
// strafe (the player); see Movement in sim.hpp. The ability, if any, is
// cast at the aim target whenever MP and its cooldown allow.
//
// State:
//   {"ok": true, "timeMs", "player": {"x", "y"}, "playerSpeed", "aim",
//    "target": {"x", "y", "radius"} (the aim target),
//    "targets": [{"x", "y", "radius", "defense", "hits", "damage",
//                 "abilityDamage", "statuses", "hp", "kills", "killedMs"}],
//    "hits": [{"target", "damage", "from"}] since the last state,
//    "history": {"bucketMs", "from", "damage": [...], "mp": [...]} with the
//               buckets since the last state, "from" the first one's index,
//    "shots": [{"subattack", "from", "projectileId", "objectId", "x", "y",
//               "heading", "ageMs", "hit"}],
//    "totals": {...}, "perSubattack": [{...}],
//    "mp", "mpMax", "statuses", "ability": {"casts", "hits", "damage"},
//    "procs": {"activations", "hits", "damage"},
//    "effects": [{"kind", "x", "y", "radius", "armed", "chain": [{"x", "y"}]}]}
// with "from" one of weapon, ability and proc (the weapon's on-shoot
// effects), shots from the ability and procs having subattack -1, and effect
// kinds poison, trap, blast and bolt.
// with counters {"volleys", "projectiles", "hits", "damage",
// "resolvedVolleys", "resolvedHits"}.
std::string Api::AdvanceSimulation(double ms) {
  if (!simulation_) {
    return Error("no simulation running").dump();
  }
  simulation_->Advance(std::max(0.0, ms));
  json shots = json::array();
  static constexpr const char* kFrom[] = {"weapon", "ability", "proc"};
  for (const Simulation::Shot& s : simulation_->Shots()) {
    shots.push_back({{"subattack", s.subattack},
                     {"from", kFrom[static_cast<int>(s.from)]},
                     {"projectileId", s.projectile_id},
                     {"objectId", s.object_id},
                     {"x", s.position.x},
                     {"y", s.position.y},
                     {"heading", s.heading_degrees},
                     {"ageMs", s.age_ms},
                     {"hit", s.hit}});
  }
  json per = json::array();
  for (const Simulation::Counters& c : simulation_->PerSubattack()) {
    per.push_back(ToJson(c));
  }
  const Point target = simulation_->TargetCenter();
  const Scenario& world = simulation_->World();
  json targets = json::array();
  for (std::size_t i = 0; i < world.targets.size(); ++i) {
    const SimTarget& t = world.targets[i];
    const Simulation::TargetCounters& c = simulation_->PerTarget()[i];
    targets.push_back({{"x", t.position.x}, {"y", t.position.y}, {"radius", t.radius},
                       {"defense", t.target.defense}, {"hits", c.hits}, {"damage", c.damage},
                       {"abilityDamage", c.ability_damage}, {"statuses", simulation_->TargetStatuses(i)},
                       {"hp", c.hp}, {"kills", c.kills},
                       {"killedMs", c.killed_ms < 0 ? json(nullptr) : json(c.killed_ms)}});
  }
  json hits = json::array();
  for (const Simulation::HitEvent& h : simulation_->RecentHits()) {
    hits.push_back({{"target", h.target}, {"damage", h.damage}, {"from", kFrom[static_cast<int>(h.from)]}});
  }
  const auto& damage_history = simulation_->DamageHistory();
  const auto& mp_history = simulation_->MpHistory();
  const auto since = static_cast<std::ptrdiff_t>(history_sent_);
  const json history = {{"bucketMs", Simulation::kHistoryMs},
                        {"from", history_sent_},
                        {"damage", std::vector<double>(damage_history.begin() + since, damage_history.end())},
                        {"mp", std::vector<double>(mp_history.begin() + since, mp_history.end())}};
  history_sent_ = damage_history.size();
  json effects = json::array();
  for (const Simulation::Effect& e : simulation_->Effects()) {
    static constexpr const char* kKinds[] = {"poison", "trap", "blast", "bolt"};
    json chain = json::array();
    for (const Point& p : e.chain) {
      chain.push_back(ToJson(p));
    }
    effects.push_back({{"kind", kKinds[static_cast<int>(e.kind)]},
                       {"x", e.position.x},
                       {"y", e.position.y},
                       {"radius", e.radius},
                       {"armed", e.armed},
                       {"chain", chain}});
  }
  const Simulation::AbilityCounters& ability = simulation_->AbilityTotals();
  const Simulation::AbilityCounters& procs = simulation_->ProcTotals();
  return json{{"ok", true},
              {"timeMs", simulation_->TimeMs()},
              {"player", ToJson(world.player)},
              {"playerSpeed", simulation_->PlayerSpeed()},
              {"target", {{"x", target.x}, {"y", target.y}, {"radius", simulation_->Radius()}}},
              {"aim", world.aim},
              {"targets", targets},
              {"shots", shots},
              {"totals", ToJson(simulation_->Totals())},
              {"perSubattack", per},
              {"mp", simulation_->Mp()},
              {"mpMax", simulation_->MaxMp()},
              {"statuses", simulation_->PlayerStatuses()},
              {"ability", {{"casts", ability.casts}, {"hits", ability.hits}, {"damage", ability.damage}}},
              {"procs", {{"activations", procs.casts}, {"hits", procs.hits}, {"damage", procs.damage}}},
              {"effects", effects},
              {"hits", hits},
              {"history", history}}
      .dump();
}

}  // namespace rotmg
