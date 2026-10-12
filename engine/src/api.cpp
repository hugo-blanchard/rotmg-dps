#include "rotmg/api.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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
#include "rotmg/optimize.hpp"
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
//    "hitCurve": {"from": 0.5, "to": 10, "step": 0.25},
//    "burst": {"window": 1, "every": 10}}
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
// per volley at each distance. "burst" adds the damage dealt in a window of
// that many seconds (0 for a single moment) every so many seconds, as
// EvaluateBurst works it out.
//
// Response:
//   {"ok": true, "breakdown": {...the weapon on its own...},
//    "build": {"dps", "weaponDps", "uptimes": [...], "assumptions": [...]},
//    "ability": {...}, "sets": [{"type", "name", "pieces", "stats", "unsupported"}],
//    "enchantmentStats": [{"stat", "amount"}], "itemStats": [{"stat", "amount"}],
//    "curve": [{"defense", "dps"}],
//    "hitCurve": [{"distance", "hits"}], "hitModelGaps": ["..."],
//    "burst": {"window", "every", "damage", "weaponDamage", "abilityDamage", "casts",
//              "assumptions"}}
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
  // Stats the items' relative on-equip effects add.
  std::vector<StatBonus> item_stats;
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
  std::vector<Enchanted::Relative> relative;
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
    relative = en.relative;
    en.weapon.unsupported.insert(en.weapon.unsupported.end(), en.unsupported.begin(), en.unsupported.end());
    r.enchanted = std::make_shared<const Weapon>(std::move(en.weapon));
    r.weapon = r.enchanted.get();
  }
  // The items' own on-equip effects besides flat stats.
  std::vector<Enchanted::Relative> item_relative;
  double discount = 1;
  const auto equip = [&](const std::vector<Activate>& on_equip) {
    for (const Activate& a : on_equip) {
      if (a.type == "IncrementStatRelative" || a.type == "BonusStatRelative") {
        item_relative.push_back({a.Text("stat"), a.Text("statRelativeTo"), a.Number("amount").value_or(0),
                                 a.type == "IncrementStatRelative"});
      } else if (a.type == "AbilityUseDiscount") {
        // RealmEye, The Twilight Gemstone: its cut stacks with other gear's.
        discount *= a.Number("multiplier").value_or(1);
      } else if (const std::string tag = "ActivateOnEquip " + a.type;
                 std::ranges::find(r.gear_unsupported, tag) == r.gear_unsupported.end()) {
        r.gear_unsupported.push_back(tag);
      }
    }
  };
  if (r.weapon != nullptr) {
    equip(r.weapon->on_equip);
  }
  if (r.ability != nullptr) {
    equip(r.ability->on_equip);
  }
  for (const Gear* g : {r.armor, r.ring}) {
    if (g != nullptr) {
      equip(g->on_equip);
    }
  }
  if (discount != 1 && r.ability != nullptr && r.ability->mp_cost) {
    Ability cheaper = *r.ability;
    cheaper.mp_cost = *cheaper.mp_cost * discount;
    r.enchanted_ability = std::make_shared<const Ability>(std::move(cheaper));
    r.ability = r.enchanted_ability.get();
  }
  // Relative bonuses from enchantments and items all come from the stats
  // before any of them, so they don't feed each other.
  if ((!relative.empty() || !item_relative.empty()) && r.weapon != nullptr) {
    const auto class_id = req.find("class");
    const auto found = std::ranges::find(
        dataset.classes, class_id != req.end() && class_id->is_string() ? class_id->get<std::string>() : "", &Class::id);
    const Class* cls = found == dataset.classes.end() ? nullptr : &*found;
    const bool of_bonus = !relative.empty() || std::ranges::any_of(item_relative, [](const auto& x) {
      return !x.of_total;
    });
    if (cls == nullptr && of_bonus) {
      r.gear_unsupported.emplace_back("relative stat bonuses, which need the class");
    }
    Character everything = r.character.With(r.weapon->stat_bonuses);
    if (r.ability != nullptr) {
      everything = everything.With(r.ability->stat_bonuses);
    }
    const std::vector<StatBonus> gains = RelativeBonuses(relative, cls, everything);
    r.item_stats = RelativeBonuses(item_relative, cls, everything);
    r.character = r.character.With(gains).With(r.item_stats);
    for (const StatBonus& g : gains) {
      const auto same = std::ranges::find(r.enchantment_stats, g.stat, &StatBonus::stat);
      if (same == r.enchantment_stats.end()) {
        r.enchantment_stats.push_back(g);
      } else {
        same->amount += g.amount;
      }
    }
    if (!relative.empty() && cls != nullptr) {
      r.notes.emplace_back(
          "relative enchantments count stats above the class's max as bonus stats, as on a maxed character, and "
          "round down");
    }
    if (!r.item_stats.empty()) {
      r.notes.emplace_back("an item's share of one stat added to another rounds down, and counts with relative "
                           "enchantments in one step");
    }
  }
  // The other gear's on-shoot effects go off as the weapon fires, like its
  // own, and some create their projectiles.
  const auto join = [&](const std::vector<Activate>& on_shoot, const std::map<int, Projectile>& created,
                        const Ability* own) {
    if (on_shoot.empty() || r.weapon == nullptr) {
      return;
    }
    Weapon w = *r.weapon;
    for (Activate a : on_shoot) {
      // An ability's shots without a `type` are its own projectile, which
      // the weapon knows by the ability's type.
      if (own != nullptr && a.Text("type").empty() && !own->projectiles.empty()) {
        a.params["type"] = std::to_string(own->type);
        w.created_projectiles.emplace(own->type, own->projectiles[0]);
      }
      w.on_shoot.push_back(std::move(a));
    }
    w.created_projectiles.insert(created.begin(), created.end());
    r.enchanted = std::make_shared<const Weapon>(std::move(w));
    r.weapon = r.enchanted.get();
  };
  if (r.ability != nullptr) {
    join(r.ability->on_shoot, r.ability->created_projectiles, r.ability);
  }
  for (const Gear* g : {r.armor, r.ring}) {
    if (g != nullptr) {
      join(g->on_shoot, g->created_projectiles, nullptr);
    }
  }
  // What the other gear does when the ability is used goes with the
  // ability's own on-use effects.
  if (r.ability != nullptr) {
    Ability with = *r.ability;
    const auto add = [&](const std::vector<Activate>& on_ability, const std::map<int, Projectile>& created) {
      with.on_ability.insert(with.on_ability.end(), on_ability.begin(), on_ability.end());
      with.created_projectiles.insert(created.begin(), created.end());
    };
    if (const Weapon* w = FindWeapon(dataset, req.at("weapon").get<std::string>())) {
      add(w->on_ability, w->created_projectiles);
    }
    for (const Gear* g : {r.armor, r.ring}) {
      if (g != nullptr) {
        add(g->on_ability, g->created_projectiles);
      }
    }
    if (with.on_ability.size() != r.ability->on_ability.size()) {
      r.enchanted_ability = std::make_shared<const Ability>(std::move(with));
      r.ability = r.enchanted_ability.get();
    }
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


// A burst window from {"window", "every"}, in seconds. Throws on one that
// isn't a window at all.
BurstWindow ParseBurst(const json& j) {
  const BurstWindow w{j.value("window", 0.0), j.value("every", 10.0)};
  if (!(w.length_s >= 0 && w.length_s <= 60 && w.every_s > w.length_s && w.every_s <= 600)) {
    throw std::invalid_argument("burst window of " + json(w.length_s).dump() + " s every " + json(w.every_s).dump() +
                                " s");
  }
  return w;
}

json ToJson(const BurstBreakdown& b, const BurstWindow& w) {
  return {{"window", w.length_s},
          {"every", w.every_s},
          {"damage", b.damage},
          {"weaponDamage", b.weapon_damage},
          {"abilityDamage", b.ability_damage},
          {"casts", b.casts},
          {"windowCasts", b.window_casts},
          {"paidCasts", b.paid_casts},
          {"assumptions", b.assumptions}};
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
                 enchantment_stats, notes, armor, ring, gear_unsupported, item_stats] = r;
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
    for (const auto& [key, list] : {std::pair{"enchantmentStats", &enchantment_stats}, {"itemStats", &item_stats}}) {
      res[key] = json::array();
      for (const StatBonus& b : *list) {
        res[key].push_back({{"stat", b.stat}, {"amount", b.amount}});
      }
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
    if (auto it = req.find("burst"); it != req.end()) {
      const BurstWindow window = ParseBurst(*it);
      res["burst"] = ToJson(EvaluateBurst(*weapon, ability, character, target, assumptions, window), window);
    }
    return res.dump();
  } catch (const std::exception& e) {
    return Error(std::string("bad request: ") + e.what()).dump();
  }
}

// A choices request is an evaluate request plus
//   "choices": {"slot": "weapon" | "ability" | "armor" | "ring",
//               "options": [{"item": "...", "enchantments": [...]}, ...]}
// where each option puts that item, or none for "", in the slot with those
// enchantments on it. Returns
//   {"ok": true, "dps": <the request's own>, "options": [<dps or null>, ...]}
// in the same order, null for an option the engine rejects, like an
// enchantment the item can't hold.
std::string Api::EvaluateChoices(std::string_view request_json) const {
  if (!dataset_) {
    return Error("no dataset loaded").dump();
  }
  try {
    json base = json::parse(request_json);
    const json choices = base.value("choices", json::object());
    for (const char* key : {"choices", "curve", "hitCurve", "burst"}) {
      base.erase(key);
    }
    const std::string slot = choices.value("slot", std::string());
    if (slot != "weapon" && slot != "ability" && slot != "armor" && slot != "ring") {
      return Error("no slot named \"" + slot + "\"").dump();
    }
    const auto dps = [&](const json& r) -> json {
      try {
        const Request p = ParseRequest(r, *dataset_);
        if (p.weapon != nullptr && !p.unknown_ability) {
          return EvaluateBuild(*p.weapon, p.ability, p.character, p.target, p.assumptions).dps;
        }
      } catch (const std::exception&) {
        // Rejected, like an enchantment the item can't hold.
      }
      return nullptr;
    };
    json out = json::array();
    for (const json& option : choices.value("options", json::array())) {
      json r = base;
      const std::string item = option.value("item", std::string());
      if (item.empty()) {
        r.erase(slot);
      } else {
        r[slot] = item;
      }
      const json enchantments = option.value("enchantments", json::array());
      if (slot == "weapon") {
        r["enchantments"] = enchantments;
      } else {
        r["gearEnchantments"][slot] = enchantments;
      }
      out.push_back(dps(r));
    }
    return json{{"ok", true}, {"dps", dps(base)}, {"options", out}}.dump();
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
//                "enchantments": "keep" | "none" | "best", "slots": 2, "artifacts": true,
//                "around": "every stat" | "nothing" | "ATT" | ...,
//                "weights": {"weapon": 1, "ability": 1}, "burst": {"window": 1, "every": 10},
//                "thorough": false, "results": 5}
// "lock" keeps the request's items in those slots. "pools" says which other
// items may be tried: tiered ones, set pieces, and the rest (untiered).
// Reskins and unobtainable items are never tried, but the request's own
// stay unless something beats them.
// "enchantments" keeps the request's where they still fit, removes them, or
// fills up to "slots" per item (2 unless set, at most 4), and no more than
// the item has, with the best ones. "artifacts" lets it use enchantments
// only an artifact or engraving puts on an item. "around" says which
// stats, as in kOptimizerStats, the search starts again from the ability
// that scales best with: every one (the default), none, which is quicker,
// or just one. Builds are ranked by their weapon DPS, with the ability's
// buffs, and the ability's own DPS, each times its weight, which is 1 for
// both unless set. With "burst", they're ranked by the weapon's and the
// ability's damage in each burst window instead, weighted the same way.
// "thorough" searches much longer, for checking the normal search. Returns
//   {"ok": true, "baseline", "evaluated",
//    "builds": [{"score", "dps", "weaponDps", "abilityDps", "burst",
//                "weapon", "ability", "armor", "ring",
//                "enchantments": {"weapon": [...], "ability", "armor", "ring"}}]}
// with the best build first and the next best, with other items, after it.
// "score" is what builds are ranked by and "baseline" is the request's own
// build's score. "burst" is there with a burst window, shaped as in
// Evaluate's response. Each progress report is
//   {"done", "baseline", "evaluated", "builds"}
// with "done" from 0 to 1, a rough guess, and the best builds so far.
//
// ExplainBuild takes the same request and returns, for the request's own
// build in the units the search ranks by,
//   {"ok": true, "score", "items": {"ability": <loss without it>, "armor", "ring"},
//    "enchantments": {"weapon": [<loss without each>], ...},
//    "stats": [{"stat", "perPoint"}], "burst"}
// with null for a loss the engine can't work out, and "burst" only with a
// burst window.
//
// EstimateOptimize takes the same request and returns
//   {"ok": true, "evaluationS", "evaluations": [low, high], "seconds": [low, high]}
// with how long one evaluation takes here and the range most searches like
// this one fall in, or the exact numbers for a search that ends while it's
// timed. It doesn't count "thorough".
namespace {

// Roughly how long a search takes: how long one evaluation takes here,
// from running the search itself for a moment, times how many evaluations
// searches like it take. Those numbers come from searching for every class
// at its max stats with each option changed in turn from the defaults: the
// range most classes fell in, what each option multiplied that by on most,
// and how much more an evaluation cost over a whole search than in its
// first moment.
json Estimate(const Dataset& d, const OptimizerProblem& problem, bool burst) {
  using Clock = std::chrono::steady_clock;
  constexpr double kTestS = 0.2;
  struct Enough {};
  const auto begin = Clock::now();
  const auto since = [&] { return std::chrono::duration<double>(Clock::now() - begin).count(); };
  OptimizerProblem test = problem;
  long evaluated = 0;
  test.progress_every_s = 0.02;
  test.progress = [&](double, const OptimizerResult& so_far) {
    evaluated = so_far.evaluated;
    if (since() >= kTestS) {
      throw Enough{};
    }
  };
  try {
    if (const auto done = SearchBuilds(d, test)) {
      // Done already: that's how long it takes.
      const double seconds = since();
      return {{"ok", true},
              {"evaluationS", seconds / static_cast<double>(std::max(1L, done->evaluated))},
              {"evaluations", {done->evaluated, done->evaluated}},
              {"seconds", {seconds, seconds}}};
    }
    return Error("the build to start from doesn't evaluate");
  } catch (const Enough&) {
  }
  // On several threads the search's first moment is the busiest, so an
  // evaluation costs about twice as much over the whole search as in it.
  constexpr double kLaterCost = 2.0;
  const double each = kLaterCost * since() / static_cast<double>(std::max(1L, evaluated));

  double low = 140000;
  double high = 360000;
  double factor = 1;
  if (problem.enchantments != OptimizerProblem::Enchantments::kBest) {
    low = 1500;
    high = 3000;
  } else {
    constexpr std::array<double, 5> kPerItem = {0.01, 0.54, 1, 1.2, 1.33};
    factor *= kPerItem[static_cast<std::size_t>(std::clamp(problem.per_item, 0, 4))];
  }
  const int restarts = static_cast<int>(std::ranges::count(problem.restart_stats, true));
  factor *= restarts == 0 ? 0.73 : restarts == 1 ? 0.8 : 1;
  if (problem.candidates[1].size() <= 1) {
    factor *= 0.32;
  }
  if (burst) {
    factor *= 1.23;
  }
  low *= factor;
  high *= factor;
  return {{"ok", true}, {"evaluationS", each}, {"evaluations", {low, high}}, {"seconds", {low * each, high * each}}};
}

// What RunOptimize does with the request: search, estimate how long a
// search would take, or explain the request's own build.
enum class Run { kSearch, kEstimate, kExplain };

std::string RunOptimize(const Dataset& d, std::string_view request_json,
                        const std::function<void(const std::string&)>& progress, Run run) {
  try {
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
    const int max_slots = std::clamp(opt.value("slots", 2), 0, 4);
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
    // A reskin plays exactly like its base, so only the base is tried, and
    // no one can get an unobtainable item. Either stays when it's the
    // request's own.
    const auto offer = [&](std::size_t slot, const auto& item) {
      if (item.reskin_of.empty() && !item.unobtainable && pools.contains(pool(item.type, item.tier))) {
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

    OptimizerProblem problem;
    problem.candidates = candidates;
    OptimizerBuild& start = problem.start;
    for (std::size_t i = 0; i < kSlots.size(); ++i) {
      start.items[i] = base.value(kSlots[i], std::string());
      if (locked.contains(kSlots[i]) || candidates[i].empty()) {
        problem.candidates[i] = {start.items[i]};
      } else if (start.items[i].empty() && run != Run::kExplain) {
        start.items[i] = candidates[i].front();
      }
    }
    // A build explained is the request's own, enchantments and all.
    if (mode == "keep" || run == Run::kExplain) {
      start.enchants[0] = base.value("enchantments", std::vector<std::string>{});
      const json gear = base.value("gearEnchantments", json::object());
      for (std::size_t i = 1; i < kSlots.size(); ++i) {
        start.enchants[i] = gear.value(kSlots[i], std::vector<std::string>{});
      }
    }
    problem.enchantments = mode == "best"   ? OptimizerProblem::Enchantments::kBest
                           : mode == "none" ? OptimizerProblem::Enchantments::kNone
                                            : OptimizerProblem::Enchantments::kKeep;
    problem.per_item = max_slots;
    problem.artifacts = artifacts;
    problem.results = results;
    problem.thorough = opt.value("thorough", false);
    const std::string around = opt.value("around", std::string("every stat"));
    for (std::size_t i = 0; i < kOptimizerStats.size(); ++i) {
      problem.restart_stats[i] = around == "every stat" || around == kOptimizerStats[i];
    }
    if (around != "every stat" && around != "nothing" &&
        std::ranges::find(kOptimizerStats, around) == kOptimizerStats.end()) {
      return Error("can't build around \"" + around + "\"").dump();
    }
    // The request's stats in kOptimizerStats order, for adding to.
    constexpr std::array<const char*, 8> kStatKeys = {"attack",   "dexterity", "speed",        "wisdom",
                                                      "vitality", "defense",   "maxHitPoints", "maxMagicPoints"};
    const json weights = opt.value("weights", json::object());
    const double weapon_weight = std::max(0.0, weights.value("weapon", 1.0));
    const double ability_weight = std::max(0.0, weights.value("ability", 1.0));
    if (weapon_weight == 0 && ability_weight == 0) {
      return Error("the weapon and ability weights can't both be 0").dump();
    }
    std::optional<BurstWindow> burst;
    if (auto it = opt.find("burst"); it != opt.end()) {
      burst = ParseBurst(*it);
    }
    // The build as a request the engine has read, or nothing for one it
    // rejects.
    const auto read = [&](const OptimizerBuild& b, const std::array<int, 8>& extra) -> std::optional<Request> {
      json r = base;
      for (std::size_t i = 0; i < extra.size(); ++i) {
        if (extra[i] != 0) {
          r[kStatKeys[i]] = r.value(kStatKeys[i], 0) + extra[i];
        }
      }
      for (std::size_t i = 0; i < kSlots.size(); ++i) {
        if (b.items[i].empty()) {
          r.erase(kSlots[i]);
        } else {
          r[kSlots[i]] = b.items[i];
        }
      }
      r["enchantments"] = b.enchants[0];
      r["gearEnchantments"] = {{"ability", b.enchants[1]}, {"armor", b.enchants[2]}, {"ring", b.enchants[3]}};
      try {
        Request p = ParseRequest(r, d);
        if (p.weapon != nullptr && !p.unknown_ability) {
          return p;
        }
      } catch (const std::exception&) {
        // A combination the engine rejects, like an enchantment that no
        // longer fits, just doesn't count.
      }
      return std::nullopt;
    };
    const auto evaluate = [](const Request& p) {
      return EvaluateBuild(*p.weapon, p.ability, p.character, p.target, p.assumptions);
    };
    const auto evaluate_burst = [&](const Request& p) {
      return EvaluateBurst(*p.weapon, p.ability, p.character, p.target, p.assumptions, *burst);
    };
    const auto ability_dps = [](const BuildBreakdown& b) { return b.ability ? b.ability->dps : 0.0; };
    problem.score = [&](const OptimizerBuild& b, const std::array<int, 8>& extra) -> std::optional<double> {
      const std::optional<Request> p = read(b, extra);
      if (!p) {
        return std::nullopt;
      }
      // Ties, common when a weight is 0, go to the build with the most
      // damage in all: a millionth of it is enough to tell them apart.
      constexpr double kTieBreak = 1e-6;
      if (burst) {
        const BurstBreakdown e = evaluate_burst(*p);
        return weapon_weight * e.weapon_damage + ability_weight * e.ability_damage + kTieBreak * e.damage;
      }
      const BuildBreakdown e = evaluate(*p);
      return weapon_weight * e.weapon_dps + ability_weight * ability_dps(e) + kTieBreak * e.dps;
    };

    const auto builds = [&](const OptimizerResult& result) {
      json out = json::array();
      for (const auto& [score, b] : result.builds) {
        json enchants = json::object();
        json entry = {{"score", score}};
        if (const std::optional<Request> p = read(b, {})) {
          const BuildBreakdown e = evaluate(*p);
          entry["dps"] = e.dps;
          entry["weaponDps"] = e.weapon_dps;
          entry["abilityDps"] = ability_dps(e);
          if (burst) {
            entry["burst"] = ToJson(evaluate_burst(*p), *burst);
          }
        }
        for (std::size_t slot = 0; slot < kSlots.size(); ++slot) {
          entry[kSlots[slot]] = b.items[slot];
          enchants[kSlots[slot]] = b.enchants[slot];
        }
        entry["enchantments"] = enchants;
        out.push_back(entry);
      }
      return out;
    };
    if (run == Run::kEstimate) {
      return Estimate(d, problem, burst.has_value()).dump();
    }
    if (run == Run::kExplain) {
      // What the build scores, what it loses without each item but the
      // weapon and each enchantment, and what a point of each stat adds,
      // measured over 10 points since some effects grow in steps of 10.
      const OptimizerBuild& b = problem.start;
      const std::optional<double> score = problem.score(b, {});
      if (!score) {
        return Error("the build doesn't evaluate").dump();
      }
      const auto loss = [&](const OptimizerBuild& without) -> json {
        const std::optional<double> s = problem.score(without, {});
        return s ? json(*score - *s) : json(nullptr);
      };
      json items = json::object();
      json enchants = json::object();
      for (std::size_t i = 0; i < kSlots.size(); ++i) {
        if (i > 0 && !b.items[i].empty()) {
          OptimizerBuild without = b;
          without.items[i].clear();
          without.enchants[i].clear();
          items[kSlots[i]] = loss(without);
        }
        enchants[kSlots[i]] = json::array();
        for (std::size_t k = 0; k < b.enchants[i].size(); ++k) {
          OptimizerBuild without = b;
          without.enchants[i].erase(without.enchants[i].begin() + static_cast<std::ptrdiff_t>(k));
          enchants[kSlots[i]].push_back(loss(without));
        }
      }
      constexpr int kStep = 10;
      json stats = json::array();
      for (std::size_t i = 0; i < kOptimizerStats.size(); ++i) {
        std::array<int, 8> extra{};
        extra[i] = kStep;
        const std::optional<double> s = problem.score(b, extra);
        stats.push_back({{"stat", kOptimizerStats[i]}, {"perPoint", s ? (*s - *score) / kStep : 0}});
      }
      json out = {{"ok", true}, {"score", *score}, {"items", items}, {"enchantments", enchants}, {"stats", stats}};
      if (const std::optional<Request> p = read(b, {}); p && burst) {
        out["burst"] = ToJson(evaluate_burst(*p), *burst);
      }
      return out.dump();
    }
    if (progress) {
      problem.progress = [&](double done, const OptimizerResult& so_far) {
        progress(json{{"done", done},
                      {"baseline", so_far.baseline},
                      {"evaluated", so_far.evaluated},
                      {"builds", builds(so_far)}}
                     .dump());
      };
    }

    const std::optional<OptimizerResult> found = SearchBuilds(d, problem);
    if (!found) {
      return Error("the build to start from doesn't evaluate").dump();
    }
    return json{{"ok", true},
                {"baseline", found->baseline},
                {"evaluated", found->evaluated},
                {"builds", builds(*found)}}
        .dump();
  } catch (const std::exception& e) {
    return Error(std::string("bad request: ") + e.what()).dump();
  }
}

}  // namespace

std::string Api::Optimize(std::string_view request_json,
                          const std::function<void(const std::string&)>& progress) const {
  if (!dataset_) {
    return Error("no dataset loaded").dump();
  }
  return RunOptimize(*dataset_, request_json, progress, Run::kSearch);
}

std::string Api::EstimateOptimize(std::string_view request_json) const {
  if (!dataset_) {
    return Error("no dataset loaded").dump();
  }
  return RunOptimize(*dataset_, request_json, {}, Run::kEstimate);
}

std::string Api::ExplainBuild(std::string_view request_json) const {
  if (!dataset_) {
    return Error("no dataset loaded").dump();
  }
  return RunOptimize(*dataset_, request_json, {}, Run::kExplain);
}

}  // namespace rotmg
