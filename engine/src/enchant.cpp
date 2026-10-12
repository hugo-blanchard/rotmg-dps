#include "rotmg/enchant.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace rotmg {
namespace {

bool Has(const std::vector<std::string>& list, const std::string& item) {
  return std::ranges::find(list, item) != list.end();
}

// -1 or no attribute means every projectile or subattack.
bool Targets(const Mutator& m, const char* key, int index) {
  const auto n = m.attributes.Number(key);
  return !n || *n < 0 || static_cast<int>(*n) == index;
}

// Awakened enchantments: the container's subattack `subAttackIndex` goes
// into the weapon's subattack slot given by the element's text, replacing
// it (Set) or pushing the rest along (Add). Spellsword Mastery shows the
// order: its container has only subattack 0 and the text says 1. The
// container's projectile joins the weapon under a fresh id.
bool TakeSubattack(Weapon& w, const Enchantment& e, const Mutator& m) {
  const auto c = e.containers.find(m.attributes.Text("subAttackObject"));
  const auto from = static_cast<std::size_t>(m.attributes.Number("subAttackIndex").value_or(0));
  if (c == e.containers.end() || from >= c->second.subattacks.size()) {
    return false;
  }
  Subattack s = c->second.subattacks[from];
  const auto p = std::ranges::find(c->second.projectiles, s.projectile_id, &Projectile::id);
  if (p == c->second.projectiles.end()) {
    return false;
  }
  Projectile copy = *p;
  copy.id = 0;
  for (const Projectile& existing : w.projectiles) {
    copy.id = std::max(copy.id, existing.id + 1);
  }
  s.projectile_id = copy.id;
  w.projectiles.push_back(std::move(copy));
  const auto to = std::min(static_cast<std::size_t>(m.number.value_or(0)), w.subattacks.size());
  if (m.tag == "SetSubAttack" && to < w.subattacks.size()) {
    w.subattacks[to] = s;
  } else {
    w.subattacks.insert(w.subattacks.begin() + static_cast<std::ptrdiff_t>(to), s);
  }
  for (const std::string& tag : c->second.unsupported) {
    if (!Has(w.unsupported, tag)) {
      w.unsupported.push_back(tag);
    }
  }
  return true;
}

int ClassMax(const Class& c, const std::string& stat) {
  if (stat == "ATT") return c.attack.max;
  if (stat == "DEX") return c.dexterity.max;
  if (stat == "WIS") return c.wisdom.max;
  if (stat == "VIT") return c.vitality.max;
  if (stat == "DEF") return c.defense.max;
  if (stat == "SPD") return c.speed.max;
  if (stat == "MAXHP") return c.max_hit_points.max;
  if (stat == "MAXMP") return c.max_magic_points.max;
  return 0;
}

}  // namespace

std::vector<StatBonus> RelativeBonuses(const std::vector<Enchanted::Relative>& relative, const Class& cls,
                                       const Character& character) {
  std::map<std::string, double> gains;
  for (const Enchanted::Relative& r : relative) {
    const int bonus = std::max(0, character.Stat(r.from) - ClassMax(cls, r.from));
    gains[r.stat] += r.percent / 100 * bonus;
  }
  std::vector<StatBonus> out;
  for (const auto& [stat, amount] : gains) {
    if (const int whole = static_cast<int>(std::floor(amount + 1e-9)); whole != 0) {
      out.push_back({stat, whole});
    }
  }
  return out;
}

bool Fits(const Enchantment& e, const Weapon& w) {
  return !e.compatible.empty() &&
         std::ranges::all_of(e.compatible, [&](const std::string& l) { return Has(w.labels, l); }) &&
         std::ranges::none_of(e.incompatible, [&](const std::string& l) { return Has(w.labels, l); }) &&
         !Has(e.incompatible_items, w.id);
}

bool Conflict(const Enchantment& a, const Enchantment& b) {
  const auto meets = [](const Enchantment& x, const Enchantment& y) {
    return std::ranges::any_of(x.incompatible_labels, [&](const std::string& l) { return Has(y.labels, l); });
  };
  return meets(a, b) || meets(b, a);
}

Enchanted Enchant(const Weapon& weapon, const std::vector<const Enchantment*>& enchantments) {
  Enchanted out{weapon, {}, {}, {}};
  Weapon& w = out.weapon;
  std::map<std::string, double> stats;
  for (const Enchantment* e : enchantments) {
    bool modelled = true;
    for (const Mutator& m : e->mutators) {
      const double n = m.number.value_or(1);
      if (m.tag == "MultiplyMinDamage") {
        // Min and max always move together in the data; one counts.
        if (!Targets(m, "projectileId", -1)) {
          modelled = false;
        }
        w.damage_bonus *= n;
      } else if (m.tag == "MultiplyMaxDamage") {
      } else if (m.tag == "MultiplyRateOfFire") {
        for (std::size_t i = 0; i < w.subattacks.size(); ++i) {
          if (Targets(m, "subAttackIndex", static_cast<int>(i))) {
            w.subattacks[i].rate_of_fire *= n;
          }
        }
      } else if (m.tag == "MultiplySpeed" || m.tag == "MultiplyLifetimeMS") {
        for (Projectile& p : w.projectiles) {
          if (Targets(m, "projectileId", p.id)) {
            (m.tag == "MultiplySpeed" ? p.speed : p.lifetime_ms) *= n;
          }
        }
      } else if (m.tag == "SetSubAttack" || m.tag == "AddSubAttack") {
        modelled = TakeSubattack(w, *e, m) && modelled;
      } else if (m.tag == "AddOnPlayerShootActivate") {
        Activate effect = m.attributes;
        effect.type = m.text;
        w.on_shoot.push_back(std::move(effect));
      } else if (m.tag == "ActivateOnEquip" && m.text == "BonusStatRelative") {
        out.relative.push_back({m.attributes.Text("stat"), m.attributes.Text("statRelativeTo"),
                                m.attributes.Number("amount").value_or(0)});
      } else if (m.tag == "ActivateOnEquip" && m.text == "IncrementStat") {
        stats[m.attributes.Text("stat")] += m.attributes.Number("amount").value_or(0);
      } else {
        modelled = false;
      }
    }
    if (!modelled) {
      out.unsupported.push_back(e->name + " (enchantment)");
    }
  }
  for (const auto& [stat, amount] : stats) {
    if (const int whole = static_cast<int>(std::floor(amount + 1e-9)); whole != 0) {
      out.stats.push_back({stat, whole});
    }
  }
  return out;
}

}  // namespace rotmg
