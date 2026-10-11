#include "rotmg/api.hpp"

#include <algorithm>
#include <exception>

#include <nlohmann/json.hpp>

#include "rotmg/build.hpp"
#include "rotmg/dataset.hpp"
#include "rotmg/evaluate.hpp"
#include "rotmg/hits.hpp"

// Request:
//   {"weapon": "Doom Bow", "ability": "Magic Quiver",
//    "attack": 50, "dexterity": 50, "wisdom": 50, "vitality": 40,
//    "defense": 25, "speed": 50, "maxHitPoints": 700, "maxMagicPoints": 300,
//    "status": {"weak", "damaging", "berserk", "dazed"},
//    "target": {"defense", "armored", "armorBroken", "exposed", "cursed"},
//    "hitsPerVolley": [null, 1.5], "curatedHitsPerVolley": [2, null],
//    "hitModel": {"distance": 4, "radius": 0.5},
//    "curve": {"from": 0, "to": 150, "step": 5},
//    "hitCurve": {"from": 0.5, "to": 10, "step": 0.25}}
// Only "weapon" is required. "curve" adds the build's DPS at each DEF in
// the range, with everything else in the request unchanged. "hitCurve"
// adds the geometric estimate's total hits per volley at each distance.
//
// Response:
//   {"ok": true, "breakdown": {...the weapon on its own...},
//    "build": {"dps", "weaponDps", "uptimes": [...], "assumptions": [...]},
//    "ability": {...}, "curve": [{"defense", "dps"}],
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
  return r;
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
    const auto [weapon, ability, unknown_ability, character, target, assumptions] = ParseRequest(req, *dataset_);
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
  if (!dataset_) {
    return "no dataset loaded";
  }
  try {
    const json req = json::parse(request_json);
    const Request r = ParseRequest(req, *dataset_);
    if (r.weapon == nullptr) {
      return "no weapon named \"" + req.at("weapon").get<std::string>() + "\"";
    }
    // The ability isn't simulated, but its stat bonuses are part of the build.
    const Character character = r.ability ? r.character.With(r.ability->stat_bonuses) : r.character;
    simulation_ = std::make_unique<Simulation>(*r.weapon, character, r.target, r.assumptions.distance.value_or(4.0),
                                               r.assumptions.target_radius);
    return "";
  } catch (const std::exception& e) {
    return std::string("bad request: ") + e.what();
  }
}

// State:
//   {"ok": true, "timeMs", "target": {"x", "y", "radius"},
//    "shots": [{"subattack", "projectileId", "x", "y", "heading", "ageMs", "hit"}],
//    "totals": {...}, "perSubattack": [{...}]}
// with counters {"volleys", "projectiles", "hits", "damage",
// "resolvedVolleys", "resolvedHits"}.
std::string Api::AdvanceSimulation(double ms) {
  if (!simulation_) {
    return Error("no simulation running").dump();
  }
  simulation_->Advance(std::max(0.0, ms));
  json shots = json::array();
  for (const Simulation::Shot& s : simulation_->Shots()) {
    shots.push_back({{"subattack", s.subattack},
                     {"projectileId", s.projectile_id},
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
  return json{{"ok", true},
              {"timeMs", simulation_->TimeMs()},
              {"target", {{"x", target.x}, {"y", target.y}, {"radius", simulation_->Radius()}}},
              {"shots", shots},
              {"totals", ToJson(simulation_->Totals())},
              {"perSubattack", per}}
      .dump();
}

}  // namespace rotmg
