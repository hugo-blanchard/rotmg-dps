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

}  // namespace

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
  Enchanted out{weapon, {}, {}};
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
