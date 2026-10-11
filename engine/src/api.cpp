#include "rotmg/api.hpp"

#include <exception>

#include <nlohmann/json.hpp>

#include "rotmg/dataset.hpp"
#include "rotmg/evaluate.hpp"

// Request:
//   {"weapon": "Doom Bow", "attack": 50, "dexterity": 50,
//    "status": {"weak", "damaging", "berserk", "dazed"},
//    "target": {"defense", "armored", "armorBroken", "exposed", "cursed"},
//    "hitsPerVolley": [null, 1.5],
//    "curve": {"from": 0, "to": 150, "step": 5}}
// Only "weapon" is required. "curve" adds DPS at each DEF in the range,
// with everything else in the request unchanged.
//
// Response:
//   {"ok": true, "breakdown": {...}, "curve": [{"defense", "dps"}]}
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
                          {"volleysPerSecond", s.volleys_per_second},
                          {"hitsPerVolley", s.hits_per_volley},
                          {"damagePerHit", s.damage_per_hit},
                          {"dps", s.dps}});
  }
  json terms = json::array();
  for (const Term& t : b.terms) {
    terms.push_back({{"name", t.name}, {"value", t.value}, {"source", SourceName(t.source)}, {"reference", t.reference}});
  }
  return {{"dps", b.dps},
          {"subattacks", subattacks},
          {"terms", terms},
          {"assumptions", b.assumptions},
          {"unsupported", b.unsupported}};
}

json Error(const std::string& message) {
  return {{"ok", false}, {"error", message}};
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
    const std::string name = req.at("weapon").get<std::string>();
    const Weapon* weapon = FindWeapon(*dataset_, name);
    if (weapon == nullptr) {
      return Error("no weapon named \"" + name + "\"").dump();
    }

    Character character;
    character.attack = req.value("attack", 0);
    character.dexterity = req.value("dexterity", 0);
    if (auto it = req.find("status"); it != req.end()) {
      character.status.weak = it->value("weak", false);
      character.status.damaging = it->value("damaging", false);
      character.status.berserk = it->value("berserk", false);
      character.status.dazed = it->value("dazed", false);
    }
    Target target;
    if (auto it = req.find("target"); it != req.end()) {
      target.defense = it->value("defense", 0);
      target.armored = it->value("armored", false);
      target.armor_broken = it->value("armorBroken", false);
      target.exposed = it->value("exposed", false);
      target.cursed = it->value("cursed", false);
    }
    Assumptions assumptions;
    if (auto it = req.find("hitsPerVolley"); it != req.end()) {
      for (const json& h : *it) {
        assumptions.hits_per_volley.push_back(h.is_null() ? std::nullopt : std::optional<double>(h.get<double>()));
      }
    }

    json res = {{"ok", true}, {"breakdown", ToJson(EvaluateWeapon(*weapon, character, target, assumptions))}};
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
        curve.push_back({{"defense", def}, {"dps", EvaluateWeapon(*weapon, character, t, assumptions).dps}});
      }
      res["curve"] = curve;
    }
    return res.dump();
  } catch (const std::exception& e) {
    return Error(std::string("bad request: ") + e.what()).dump();
  }
}

}  // namespace rotmg
