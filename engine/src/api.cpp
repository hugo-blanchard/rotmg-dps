#include "rotmg/api.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

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
//    "armor": "Robe of the Grand Sorcerer", "ring": "Ring of Unbound Attack",
//    "wearing": [14381, 14382], "enchantments": ["Damage_Bonus_4"], "class": "Archer",
//    "artifacts": true,
//    "hitsPerVolley": [null, 1.5], "curatedHitsPerVolley": [2, null],
//    "hitModel": {"distance": 4, "radius": 0.5},
//    "curve": {"from": 0, "to": 150, "step": 5},
//    "hitCurve": {"from": 0.5, "to": 10, "step": 0.25}}
// Only "weapon" is required. "armor" and "ring" name the armor and ring worn:
// their stats add to the character's, and they count for sets and for what
// on-shoot effects need. "wearing" lists the item types of other gear worn
// when they aren't named that way.
// "enchantments" are ids of up to 4 enchantments on the weapon, and
// "gearEnchantments" up to 4 each on {"ring", "armor", "ability"}; relative
// ones ("% of Bonus ATT") need "class", the class id. Each item takes as
// many as it has slots, of the ones it can roll; with "artifacts" (the
// default) also the ones an artifact or engraving puts on it. "curve" adds
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
          {"durationS", b.duration_s},
          {"factor", b.factor}};
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
  const Gear* armor = nullptr;
  const Gear* ring = nullptr;
  // What the armor and ring do that isn't modelled.
  std::vector<std::string> gear_unsupported;
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
  // The armor and ring add their stats like the weapon and ability do.
  std::vector<int> worn = req.value("wearing", std::vector<int>{});
  for (const auto& [key, list, into] : {std::tuple{"armor", &dataset.armors, &r.armor}, {"ring", &dataset.rings, &r.ring}}) {
    if (auto it = req.find(key); it != req.end() && !it->is_null()) {
      *into = FindGear(*list, it->get<std::string>());
      if (*into == nullptr) {
        throw std::runtime_error(std::string("no ") + key + " named \"" + it->get<std::string>() + "\"");
      }
      r.character = r.character.With((*into)->stat_bonuses);
      worn.push_back((*into)->type);
      for (const std::string& tag : (*into)->unsupported) {
        if (std::ranges::find(r.gear_unsupported, tag) == r.gear_unsupported.end()) {
          r.gear_unsupported.push_back(tag);
        }
      }
    }
  }
  // Set bonuses count the weapon and ability plus the other pieces worn,
  // and add to the character like gear does.
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
  // Reads one item's enchantment ids, checking the item can have each, that
  // none clash and that they fit in its slots.
  const bool artifacts = req.value("artifacts", true);
  const auto pick = [&](const json& ids, const std::string& item, const auto& obtained, int slots) {
    std::vector<const Enchantment*> picked;
    for (const json& id : ids) {
      const auto e = std::ranges::find(dataset.enchantments, id.get<std::string>(), &Enchantment::id);
      if (e == dataset.enchantments.end()) {
        throw std::runtime_error("no enchantment named \"" + id.get<std::string>() + "\"");
      }
      const Obtained how = obtained(*e);
      if (how == Obtained::kNever && Legacy(*e)) {
        throw std::runtime_error(e->name + " is a legacy enchantment, which can't be obtained");
      }
      if (how == Obtained::kNever && slots == 0) {
        throw std::runtime_error(item + " can't be enchanted");
      }
      if (how == Obtained::kNever) {
        throw std::runtime_error(e->name + " doesn't go on " + item);
      }
      if (how == Obtained::kArtifact && !artifacts) {
        throw std::runtime_error(e->name + " only goes on " + item + " with an artifact");
      }
      if (how == Obtained::kArtifact) {
        r.notes.push_back(e->name + " only goes on " + item + " with an artifact or an engraving");
      }
      for (const Enchantment* other : picked) {
        if (Conflict(*e, *other)) {
          throw std::runtime_error(e->name + " and " + other->name + " can't share an item");
        }
      }
      picked.push_back(&*e);
    }
    if (std::cmp_greater(picked.size(), slots)) {
      throw std::runtime_error(item + " holds at most " + std::to_string(slots) + " enchantments");
    }
    return picked;
  };
  const auto on_weapon = req.find("enchantments");
  const auto on_gear = req.find("gearEnchantments");
  if ((on_weapon != req.end() || on_gear != req.end()) && r.weapon != nullptr) {
    const std::vector<const Enchantment*> picked =
        on_weapon == req.end()
            ? std::vector<const Enchantment*>{}
            : pick(*on_weapon, r.weapon->name,
                   [&](const Enchantment& e) { return HowObtained(e, *r.weapon, dataset); },
                   EnchantmentSlots(r.weapon->enchanting, dataset));
    GearEnchantments gear;
    // A named item takes what its own labels and pool allow; otherwise any
    // item of the slot.
    const auto on_slot = [&](const char* key, const char* label, const auto* item) {
      const auto ids = on_gear->find(key);
      if (ids == on_gear->end()) {
        return;
      }
      if (item == nullptr) {
        gear.emplace_back(label, pick(*ids, std::string("your ") + key,
                                      [&](const Enchantment& e) { return HowObtainedOnSlot(e, label); }, 4));
      } else {
        gear.emplace_back(label, pick(*ids, item->name,
                                      [&](const Enchantment& e) { return HowObtained(e, *item, dataset); },
                                      EnchantmentSlots(item->enchanting, dataset)));
      }
    };
    if (on_gear != req.end()) {
      on_slot("ring", "RING", r.ring);
      on_slot("armor", "ARMOR", r.armor);
      on_slot("ability", "ABILITY", r.ability);
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
  s.lead = it->value("lead", false);
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
    if (auto w = t.find("invulnerable"); w != t.end()) {
      target.invulnerable_every_s = w->value("every", 0.0);
      target.invulnerable_s = w->value("for", 0.0);
    }
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

// The state of one simulation, as AdvanceSimulation describes it.
json StateOf(const Simulation& sim, std::size_t& history_sent) {
  json shots = json::array();
  static constexpr const char* kFrom[] = {"weapon", "ability", "proc"};
  for (const Simulation::Shot& s : sim.Shots()) {
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
  for (const Simulation::Counters& c : sim.PerSubattack()) {
    per.push_back(ToJson(c));
  }
  const Point target = sim.TargetCenter();
  const Scenario& world = sim.World();
  json targets = json::array();
  for (std::size_t i = 0; i < world.targets.size(); ++i) {
    const SimTarget& t = world.targets[i];
    const Simulation::TargetCounters& c = sim.PerTarget()[i];
    targets.push_back({{"x", t.position.x}, {"y", t.position.y}, {"radius", t.radius},
                       {"defense", t.target.defense}, {"hits", c.hits}, {"damage", c.damage},
                       {"abilityDamage", c.ability_damage}, {"statuses", sim.TargetStatuses(i)},
                       {"hp", c.hp}, {"kills", c.kills}, {"uptimeMs", sim.TargetUptime(i)},
                       {"invulnerable", sim.Invulnerable(i)},
                       {"killedMs", c.killed_ms < 0 ? json(nullptr) : json(c.killed_ms)}});
  }
  json hits = json::array();
  for (const Simulation::HitEvent& h : sim.RecentHits()) {
    hits.push_back({{"target", h.target}, {"damage", h.damage}, {"from", kFrom[static_cast<int>(h.from)]}});
  }
  const auto& damage_history = sim.DamageHistory();
  const auto& mp_history = sim.MpHistory();
  const auto since = static_cast<std::ptrdiff_t>(history_sent);
  const json history = {{"bucketMs", Simulation::kHistoryMs},
                        {"from", history_sent},
                        {"damage", std::vector<double>(damage_history.begin() + since, damage_history.end())},
                        {"mp", std::vector<double>(mp_history.begin() + since, mp_history.end())}};
  history_sent = damage_history.size();
  json effects = json::array();
  for (const Simulation::Effect& e : sim.Effects()) {
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
  const Simulation::AbilityCounters& ability = sim.AbilityTotals();
  const Simulation::AbilityCounters& procs = sim.ProcTotals();
  return json{{"ok", true},
              {"timeMs", sim.TimeMs()},
              {"player", ToJson(world.player)},
              {"playerSpeed", sim.PlayerSpeed()},
              {"target", {{"x", target.x}, {"y", target.y}, {"radius", sim.Radius()}}},
              {"aim", world.aim},
              {"targets", targets},
              {"shots", shots},
              {"totals", ToJson(sim.Totals())},
              {"perSubattack", per},
              {"mp", sim.Mp()},
              {"mpMax", sim.MaxMp()},
              {"statuses", sim.PlayerStatuses()},
              {"uptimeMs", sim.PlayerUptime()},
              {"ability", {{"casts", ability.casts}, {"hits", ability.hits}, {"damage", ability.damage}}},
              {"procs", {{"activations", procs.casts}, {"hits", procs.hits}, {"damage", procs.damage}}},
              {"effects", effects},
              {"hits", hits},
              {"history", history}}
;
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
                 enchantment_stats, notes, armor, ring, gear_unsupported] = r;
    if (weapon == nullptr) {
      return Error("no weapon named \"" + req.at("weapon").get<std::string>() + "\"").dump();
    }
    if (unknown_ability) {
      return Error("no ability named \"" + req.at("ability").get<std::string>() + "\"").dump();
    }

    const BuildBreakdown build = EvaluateBuild(*weapon, ability, character, target, assumptions);
    json res = {{"ok", true}, {"breakdown", ToJson(build.weapon)}, {"build", ToJson(build)}};
    json& listed = res["breakdown"]["unsupported"];
    for (const std::string& tag : gear_unsupported) {
      if (std::find(listed.begin(), listed.end(), json(tag)) == listed.end()) {
        listed.push_back(tag);
      }
    }
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
  comparison_.reset();
  history_sent_ = 0;
  comparison_history_sent_ = 0;
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
    const Scenario scenario = ReadScenario(req, r);
    simulation_ = std::make_unique<Simulation>(*r.weapon, character, scenario, r.ability);
    if (auto it = req.find("compare"); it != req.end() && it->is_object()) {
      // The other build goes into the same scene, set up by this request.
      const Request other = ParseRequest(*it, *dataset_);
      if (other.weapon == nullptr) {
        simulation_.reset();
        return "no weapon named \"" + it->value("weapon", std::string()) + "\" to compare with";
      }
      const Character c = other.ability ? other.character.With(other.ability->stat_bonuses) : other.character;
      comparison_ = std::make_unique<Simulation>(*other.weapon, c, scenario, other.ability);
    }
    return "";
  } catch (const std::exception& e) {
    return std::string("bad request: ") + e.what();
  }
}

// A simulation request is an evaluate request plus, optionally,
//   "scenario": {"player": {"x", "y", "movement"}, "aim": 0, "seed": 1,
//                "fullMp": true, "respawn": false, "randomRolls": false,
//                "lead": false,
//                "targets": [{"x", "y", "radius", "defense", "armored", ...,
//                             "movement", "hp",
//                             "invulnerable": {"every", "for"}}]}
// in tiles, with +y the way the player faces at the start. A movement is
// {"kind", "speed", "range", "far", "clockwise"}, kind one of still, strafe,
// circle, chargeRetreat, wander (targets), keepDistance, orbit, kite and
// strafe (the player); see Movement in sim.hpp. The ability, if any, is
// cast at the aim target whenever MP and its cooldown allow. "compare" can
// hold another evaluate request, simulated in the same scene; its state
// comes back as "compare", without the shots, effects and hits.
//
// State:
//   {"ok": true, "timeMs", "player": {"x", "y"}, "playerSpeed", "aim",
//    "target": {"x", "y", "radius"} (the aim target),
//    "targets": [{"x", "y", "radius", "defense", "hits", "damage",
//                 "abilityDamage", "statuses", "hp", "kills", "killedMs",
//                 "uptimeMs", "invulnerable"}],
//    "hits": [{"target", "damage", "from"}] since the last state,
//    "history": {"bucketMs", "from", "damage": [...], "mp": [...]} with the
//               buckets since the last state, "from" the first one's index,
//    "shots": [{"subattack", "from", "projectileId", "objectId", "x", "y",
//               "heading", "ageMs", "hit"}],
//    "totals": {...}, "perSubattack": [{...}],
//    "mp", "mpMax", "statuses", "uptimeMs": {"Berserk": ms, ...},
//    "ability": {"casts", "hits", "damage"},
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
  json state = StateOf(*simulation_, history_sent_);
  if (comparison_) {
    comparison_->Advance(std::max(0.0, ms));
    json other = StateOf(*comparison_, comparison_history_sent_);
    for (const char* drawn : {"shots", "effects", "hits"}) {
      other.erase(drawn);
    }
    state["compare"] = std::move(other);
  }
  return state.dump();
}

// A many-runs request is a simulation request plus "runs" (default 100, at
// most 1000) and "seconds" (default 60, at most 600), the longest a run goes
// on. A run stops early once every target is dead. Returns
//   {"ok": true, "runs", "dps": [...], "timeToKill": [...], "unfinished"}
// with both lists sorted, DPS counted up to the last kill, time to kill in
// seconds for the runs that killed everything, and "compare" the same for
// the build to compare with, if any.
std::string Api::SimulateMany(std::string_view request_json) const {
  if (!dataset_) {
    return Error("no dataset loaded").dump();
  }
  try {
    const json req = json::parse(request_json);
    const Request r = ParseRequest(req, *dataset_);
    if (r.weapon == nullptr) {
      return Error("no weapon named \"" + req.at("weapon").get<std::string>() + "\"").dump();
    }
    const int runs = std::clamp(req.value("runs", 100), 1, 1000);
    const double seconds = std::clamp(req.value("seconds", 60.0), 1.0, 600.0);
    Scenario scenario = ReadScenario(req, r);
    scenario.random_rolls = true;
    const auto spread = [&](const Request& build) {
      const Character c = build.ability ? build.character.With(build.ability->stat_bonuses) : build.character;
      std::vector<double> dps;
      std::vector<double> kill;
      Scenario run = scenario;
      for (int k = 0; k < runs; ++k) {
        run.seed = scenario.seed + static_cast<unsigned long long>(k);
        Simulation sim(*build.weapon, c, run, build.ability);
        const auto all_dead = [&] {
          return std::ranges::all_of(sim.PerTarget(), [](const auto& t) { return t.killed_ms >= 0; });
        };
        while (sim.TimeMs() < seconds * 1000 && !(!run.respawn && all_dead())) {
          sim.Advance(100);
        }
        double damage = 0;
        double last = 0;
        for (const auto& t : sim.PerTarget()) {
          damage += t.damage;
          last = std::max(last, t.killed_ms);
        }
        const bool done = !run.respawn && all_dead();
        const double time_s = (done ? last : sim.TimeMs()) / 1000;
        dps.push_back(time_s > 0 ? damage / time_s : 0);
        if (done) {
          kill.push_back(last / 1000);
        }
      }
      std::ranges::sort(dps);
      std::ranges::sort(kill);
      const int unfinished = runs - static_cast<int>(kill.size());
      return json{{"runs", runs}, {"dps", dps}, {"timeToKill", kill}, {"unfinished", unfinished}};
    };
    json res = spread(r);
    res["ok"] = true;
    if (auto it = req.find("compare"); it != req.end() && it->is_object()) {
      const Request other = ParseRequest(*it, *dataset_);
      if (other.weapon == nullptr) {
        return Error("no weapon named \"" + it->value("weapon", std::string()) + "\" to compare with").dump();
      }
      res["compare"] = spread(other);
    }
    return res.dump();
  } catch (const std::exception& e) {
    return Error(std::string("bad request: ") + e.what()).dump();
  }
}

// An optimize request is an evaluate request, which must name the class,
// plus
//   "optimize": {"lock": ["weapon", ...], "pools": ["tiered", "untiered", "set"],
//                "enchantments": "keep" | "none" | "best", "slots": 4, "artifacts": true,
//                "results": 5}
// "lock" keeps the request's items in those slots. "pools" says which other
// items may be tried: tiered ones, set pieces, and the rest (untiered).
// "enchantments" keeps the request's where they still fit, removes them, or
// fills up to "slots" per item, and no more than the item has, with the best
// ones. "artifacts" lets it use enchantments only an artifact or engraving
// puts on an item. Returns
//   {"ok": true, "baseline", "evaluated",
//    "builds": [{"dps", "weapon", "ability", "armor", "ring",
//                "enchantments": {"weapon": [...], "ability", "armor", "ring"}}]}
// with the best build first and the next best, with other items, after it.
// "baseline" is the request's own build's DPS.
std::string Api::Optimize(std::string_view request_json) const {
  if (!dataset_) {
    return Error("no dataset loaded").dump();
  }
  try {
    const Dataset& d = *dataset_;
    json base = json::parse(request_json);
    const json opt = base.value("optimize", json::object());
    for (const char* key : {"optimize", "curve", "hitCurve"}) {
      base.erase(key);
    }
    const auto cls = std::ranges::find(d.classes, base.value("class", std::string()), &Class::id);
    if (cls == d.classes.end()) {
      return Error("optimizing needs the class").dump();
    }
    const std::set<std::string> locked = opt.value("lock", std::set<std::string>{});
    const std::set<std::string> pools = opt.value("pools", std::set<std::string>{"tiered", "untiered", "set"});
    const std::string mode = opt.value("enchantments", std::string("keep"));
    const int max_slots = std::clamp(opt.value("slots", 4), 0, 4);
    const bool artifacts = opt.value("artifacts", true);
    base["artifacts"] = artifacts;
    const auto results = static_cast<std::size_t>(std::clamp(opt.value("results", 5), 1, 20));

    // The four slots, in the game's order, and what each can hold.
    constexpr std::array<const char*, 4> kSlots = {"weapon", "ability", "armor", "ring"};
    std::set<int> in_sets;
    for (const EquipmentSet& s : d.sets) {
      for (const EquipmentSet::Piece& p : s.pieces) {
        in_sets.insert(p.item_type);
      }
    }
    const auto pool = [&](int type, const std::optional<int>& tier) {
      return tier ? "tiered" : in_sets.contains(type) ? "set" : "untiered";
    };
    std::array<std::vector<std::string>, 4> candidates;
    // A reskin plays exactly like its base, so only the base is tried.
    const auto offer = [&](std::size_t slot, const auto& item) {
      if (item.reskin_of.empty() && pools.contains(pool(item.type, item.tier))) {
        candidates[slot].push_back(item.name);
      }
    };
    for (const Weapon& w : d.weapons) {
      if (w.slot_type == cls->weapon_slot_type) offer(0, w);
    }
    for (const Ability& a : d.abilities) {
      if (a.slot_type == cls->ability_slot_type) offer(1, a);
    }
    for (const Gear& g : d.armors) {
      if (g.slot_type == cls->armor_slot_type) offer(2, g);
    }
    for (const Gear& g : d.rings) {
      if (g.slot_type == cls->ring_slot_type) offer(3, g);
    }

    struct Build {
      std::array<std::string, 4> items;
      std::array<std::vector<std::string>, 4> enchants;
    };
    Build start;
    for (std::size_t i = 0; i < kSlots.size(); ++i) {
      start.items[i] = base.value(kSlots[i], std::string());
      if (locked.contains(kSlots[i]) || candidates[i].empty()) {
        candidates[i] = {start.items[i]};
      } else if (start.items[i].empty()) {
        start.items[i] = candidates[i].front();
      }
    }
    if (mode == "keep") {
      start.enchants[0] = base.value("enchantments", std::vector<std::string>{});
      const json gear = base.value("gearEnchantments", json::object());
      for (std::size_t i = 1; i < kSlots.size(); ++i) {
        start.enchants[i] = gear.value(kSlots[i], std::vector<std::string>{});
      }
    }

    // What can go on the item in a slot, by the game's rules, and how many,
    // once per item.
    struct Fitting {
      std::vector<const Enchantment*> options;
      int slots = 0;
    };
    std::map<std::pair<std::size_t, std::string>, Fitting> fitting_cache;
    const auto fitting = [&](std::size_t slot, const std::string& item) -> const Fitting& {
      const auto key = std::pair{slot, item};
      if (auto it = fitting_cache.find(key); it != fitting_cache.end()) {
        return it->second;
      }
      const Weapon* w = slot == 0 ? FindWeapon(d, item) : nullptr;
      const Ability* a = slot == 1 ? FindAbility(d, item) : nullptr;
      const Gear* g = slot == 2 ? FindGear(d.armors, item) : slot == 3 ? FindGear(d.rings, item) : nullptr;
      Fitting out;
      out.slots = w   ? EnchantmentSlots(w->enchanting, d)
                  : a ? EnchantmentSlots(a->enchanting, d)
                  : g ? EnchantmentSlots(g->enchanting, d)
                      : slot == 1 ? 4 : 0;
      for (const Enchantment& e : d.enchantments) {
        const Obtained how = w   ? HowObtained(e, *w, d)
                             : a ? HowObtained(e, *a, d)
                             : g ? HowObtained(e, *g, d)
                                 : slot == 1 ? HowObtainedOnSlot(e, "ABILITY") : Obtained::kNever;
        if (how == Obtained::kRolls || (artifacts && how == Obtained::kArtifact)) {
          out.options.push_back(&e);
        }
      }
      return fitting_cache.emplace(key, std::move(out)).first->second;
    };
    // Keeps the enchantments that still fit the slot's item, in order, as
    // many as it holds.
    const auto adapt = [&](Build& b, std::size_t slot) {
      if (mode == "none") {
        b.enchants[slot].clear();
        return;
      }
      const Fitting& fits = fitting(slot, b.items[slot]);
      std::erase_if(b.enchants[slot], [&](const std::string& id) {
        return std::ranges::none_of(fits.options, [&](const Enchantment* e) { return e->id == id; });
      });
      if (std::cmp_greater(b.enchants[slot].size(), fits.slots)) {
        b.enchants[slot].resize(static_cast<std::size_t>(fits.slots));
      }
    };

    long evaluated = 0;
    std::map<std::string, std::optional<double>> scores;
    std::map<std::string, std::pair<double, Build>> seen;
    const auto score = [&](const Build& b) -> std::optional<double> {
      std::string items_key;
      for (const std::string& item : b.items) {
        items_key += item + "\n";
      }
      std::string key = items_key;
      for (const auto& list : b.enchants) {
        for (const std::string& id : list) {
          key += id + ",";
        }
        key += "\n";
      }
      if (auto it = scores.find(key); it != scores.end()) {
        return it->second;
      }
      json r = base;
      for (std::size_t i = 0; i < kSlots.size(); ++i) {
        if (b.items[i].empty()) {
          r.erase(kSlots[i]);
        } else {
          r[kSlots[i]] = b.items[i];
        }
      }
      r["enchantments"] = b.enchants[0];
      r["gearEnchantments"] = {{"ability", b.enchants[1]}, {"armor", b.enchants[2]}, {"ring", b.enchants[3]}};
      ++evaluated;
      std::optional<double> dps;
      try {
        const Request p = ParseRequest(r, d);
        if (p.weapon != nullptr && !p.unknown_ability) {
          dps = EvaluateBuild(*p.weapon, p.ability, p.character, p.target, p.assumptions).dps;
        }
      } catch (const std::exception&) {
        // A combination the engine rejects, like an enchantment that no
        // longer fits, just doesn't count.
      }
      scores.emplace(key, dps);
      // One entry per set of items, with its best enchantments.
      if (auto it = seen.find(items_key); dps && (it == seen.end() || it->second.first < *dps)) {
        seen[items_key] = {*dps, b};
      }
      return dps;
    };

    // The enchantments worth trying on an item: ones that change the DPS
    // on their own. Defense, HP and loot never do. One that any item of the
    // slot takes (a generic stat bonus) is tested once per slot; one made
    // for certain items, like an awakening, once per item.
    constexpr std::array<const char*, 4> kSlotLabels = {"WEAPON", "ABILITY", "ARMOR", "RING"};
    std::map<std::pair<std::size_t, std::string>, bool> generic_cache;
    std::map<std::pair<std::size_t, std::string>, std::vector<const Enchantment*>> relevant_cache;
    const auto relevant = [&](const Build& b, std::size_t slot) -> const std::vector<const Enchantment*>& {
      const auto key = std::pair{slot, b.items[slot]};
      if (auto it = relevant_cache.find(key); it != relevant_cache.end()) {
        return it->second;
      }
      Build bare = b;
      bare.enchants[slot].clear();
      const double without = score(bare).value_or(0);
      const auto changes = [&](const Enchantment* e) {
        bare.enchants[slot] = {e->id};
        const auto s = score(bare);
        return s && std::abs(*s - without) > without * 1e-9;
      };
      std::vector<const Enchantment*> out;
      for (const Enchantment* e : fitting(slot, b.items[slot]).options) {
        if (!FitsSlot(*e, kSlotLabels[slot])) {
          if (changes(e)) out.push_back(e);
          continue;
        }
        const auto generic = std::pair{slot, e->id};
        auto it = generic_cache.find(generic);
        if (it == generic_cache.end()) {
          it = generic_cache.emplace(generic, changes(e)).first;
        }
        if (it->second) out.push_back(e);
      }
      return relevant_cache.emplace(key, std::move(out)).first->second;
    };

    // Fills the slot's item with the enchantments that add the most, one
    // at a time, while one still adds something.
    const auto fill = [&](Build& b, std::size_t slot) {
      b.enchants[slot].clear();
      double current = score(b).value_or(0);
      if (mode != "best" || b.items[slot].empty()) {
        return current;
      }
      const auto& options = relevant(b, slot);
      for (int k = 0; k < std::min(max_slots, fitting(slot, b.items[slot]).slots); ++k) {
        const Enchantment* best = nullptr;
        double best_dps = current;
        for (const Enchantment* e : options) {
          const bool taken = std::ranges::any_of(b.enchants[slot], [&](const std::string& id) {
            const auto other = std::ranges::find(d.enchantments, id, &Enchantment::id);
            return other->id == e->id || Conflict(*other, *e);
          });
          if (taken) {
            continue;
          }
          b.enchants[slot].push_back(e->id);
          if (const auto s = score(b); s && *s > best_dps * (1 + 1e-9)) {
            best_dps = *s;
            best = e;
          }
          b.enchants[slot].pop_back();
        }
        if (best == nullptr) {
          break;
        }
        b.enchants[slot].push_back(best->id);
        current = best_dps;
      }
      return current;
    };

    const auto baseline = score(start);
    if (!baseline) {
      return Error("the build to start from doesn't evaluate").dump();
    }
    Build current = start;
    double current_dps = *baseline;
    const auto fill_all = [&] {
      if (mode != "best") {
        return;
      }
      for (std::size_t i = 0; i < kSlots.size(); ++i) {
        current_dps = fill(current, i);
      }
    };

    // Coordinate descent: the best item for one slot with the others as
    // they are, slot by slot, then whole sets at once so set bonuses count,
    // until nothing improves. Items are ranked with the enchantments they
    // keep, then the best few are filled; everything is filled again between
    // rounds.
    fill_all();
    for (int round = 0; round < 6; ++round) {
      bool improved = false;
      const auto take = [&](const Build& next, double dps) {
        if (dps > current_dps * (1 + 1e-9)) {
          current = next;
          current_dps = dps;
          improved = true;
        }
      };
      for (std::size_t slot = 0; slot < kSlots.size(); ++slot) {
        std::vector<std::pair<double, Build>> tried;
        for (const std::string& name : candidates[slot]) {
          if (name == current.items[slot]) {
            continue;
          }
          Build next = current;
          next.items[slot] = name;
          adapt(next, slot);
          if (const auto s = score(next)) {
            tried.emplace_back(*s, std::move(next));
          }
        }
        std::ranges::sort(tried, [](const auto& a, const auto& b) { return a.first > b.first; });
        if (mode == "best") {
          // The best few items with their own enchantments filled in, since
          // some take enchantments others can't, like awakenings.
          for (std::size_t i = 0; i < tried.size() && i < 8; ++i) {
            tried[i].first = fill(tried[i].second, slot);
          }
          std::ranges::sort(tried, [](const auto& a, const auto& b) { return a.first > b.first; });
        }
        if (!tried.empty()) {
          take(tried.front().second, tried.front().first);
        }
      }
      for (const EquipmentSet& set : d.sets) {
        Build next = current;
        int changed = 0;
        for (const EquipmentSet::Piece& p : set.pieces) {
          const auto slot = static_cast<std::size_t>(p.slot);
          const bool usable =
              slot < kSlots.size() && std::ranges::find(candidates[slot], p.name) != candidates[slot].end();
          if (usable && next.items[slot] != p.name) {
            next.items[slot] = p.name;
            adapt(next, slot);
            ++changed;
          }
        }
        if (const auto s = changed >= 2 ? score(next) : std::nullopt) {
          take(next, *s);
        }
      }
      const double before = current_dps;
      fill_all();
      if (!improved && current_dps <= before * (1 + 1e-9)) {
        break;
      }
    }

    // Greedy filling can settle on a worse set; swapping one enchantment at
    // a time for another that fits fixes most of that.
    for (int pass = 0; mode == "best" && pass < 3; ++pass) {
      bool swapped = false;
      for (std::size_t slot = 0; slot < kSlots.size(); ++slot) {
        if (current.items[slot].empty()) {
          continue;
        }
        const auto& options = relevant(current, slot);
        for (std::size_t k = 0; k < current.enchants[slot].size(); ++k) {
          for (const Enchantment* e : options) {
            Build next = current;
            next.enchants[slot][k] = e->id;
            const bool clashes = std::ranges::any_of(next.enchants[slot], [&](const std::string& id) {
              const auto other = std::ranges::find(d.enchantments, id, &Enchantment::id);
              return other->id != e->id && Conflict(*other, *e);
            });
            const bool twice = std::ranges::count(next.enchants[slot], e->id) > 1;
            if (clashes || twice) {
              continue;
            }
            if (const auto s = score(next); s && *s > current_dps * (1 + 1e-9)) {
              current = std::move(next);
              current_dps = *s;
              swapped = true;
            }
          }
        }
      }
      if (!swapped) {
        break;
      }
    }

    std::vector<std::pair<double, Build>> ranked;
    for (auto& [key, entry] : seen) {
      ranked.push_back(entry);
    }
    std::ranges::sort(ranked, [](const auto& a, const auto& b) { return a.first > b.first; });
    json builds = json::array();
    for (std::size_t i = 0; i < ranked.size() && i < results; ++i) {
      const Build& b = ranked[i].second;
      json enchants = json::object();
      json entry = {{"dps", ranked[i].first}};
      for (std::size_t slot = 0; slot < kSlots.size(); ++slot) {
        entry[kSlots[slot]] = b.items[slot];
        enchants[kSlots[slot]] = b.enchants[slot];
      }
      entry["enchantments"] = enchants;
      builds.push_back(entry);
    }
    return json{{"ok", true}, {"baseline", *baseline}, {"evaluated", evaluated}, {"builds", builds}}.dump();
  } catch (const std::exception& e) {
    return Error(std::string("bad request: ") + e.what()).dump();
  }
}

}  // namespace rotmg
