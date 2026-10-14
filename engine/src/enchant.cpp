#include "rotmg/enchant.hpp"

#include <algorithm>
#include <cctype>

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

}  // namespace

bool FitsLabels(const Enchantment& e, const std::vector<std::string>& labels, const std::string& id) {
  return !e.compatible.empty() &&
         std::ranges::all_of(e.compatible, [&](const std::string& l) { return Has(labels, l); }) &&
         std::ranges::none_of(e.incompatible, [&](const std::string& l) { return Has(labels, l); }) &&
         !Has(e.incompatible_items, id);
}

bool Fits(const Enchantment& e, const Weapon& w) { return FitsLabels(e, w.labels, w.id); }

bool Fits(const Enchantment& e, const Gear& g) { return FitsLabels(e, g.labels, g.id); }

bool FitsSlot(const Enchantment& e, const std::string& slot_label) {
  const auto generic = [&](const std::string& l) { return l == "EQUIPMENT" || l == slot_label; };
  return !e.compatible.empty() && std::ranges::all_of(e.compatible, generic) &&
         std::ranges::none_of(e.incompatible, generic);
}

bool Legacy(const Enchantment& e) { return Has(e.labels, "LEGACY"); }

namespace {

Obtained Obtain(const Enchantment& e, const std::vector<std::string>& labels, const std::string& id,
                const Enchanting& enchanting, const Dataset& d) {
  if (!FitsLabels(e, labels, id) || Legacy(e)) {
    return Obtained::kNever;
  }
  if (d.enchantment_pools.empty()) {
    return Obtained::kRolls;
  }
  if (enchanting.slots == 0) {
    return Obtained::kNever;
  }
  // A pool the dataset doesn't list can't be checked, so the labels decide.
  const auto pool = d.enchantment_pools.find(enchanting.pool);
  return pool == d.enchantment_pools.end() || pool->second.contains(e.id) ? Obtained::kRolls : Obtained::kArtifact;
}

}  // namespace

Obtained HowObtained(const Enchantment& e, const Weapon& w, const Dataset& d) {
  return Obtain(e, w.labels, w.id, w.enchanting, d);
}

Obtained HowObtained(const Enchantment& e, const Ability& a, const Dataset& d) {
  // Abilities have no labels in datasets from before ability labels, which
  // also have no pools. In newer ones an ability without labels takes none.
  return a.labels.empty() && d.enchantment_pools.empty() ? HowObtainedOnSlot(e, "ABILITY")
                                                          : Obtain(e, a.labels, a.id, a.enchanting, d);
}

Obtained HowObtained(const Enchantment& e, const Gear& g, const Dataset& d) {
  return Obtain(e, g.labels, g.id, g.enchanting, d);
}

Obtained HowObtainedOnSlot(const Enchantment& e, const std::string& slot_label) {
  return FitsSlot(e, slot_label) && !Legacy(e) ? Obtained::kRolls : Obtained::kNever;
}

// Divine items have 4 slots, the most (RealmEye: Enchanting).
int EnchantmentSlots(const Enchanting& enchanting, const Dataset& d) {
  return d.enchantment_pools.empty() ? 4 : std::clamp(enchanting.slots, 0, 4);
}

bool Conflict(const Enchantment& a, const Enchantment& b) {
  const auto meets = [](const Enchantment& x, const Enchantment& y) {
    return std::ranges::any_of(x.incompatible_labels, [&](const std::string& l) { return Has(y.labels, l); });
  };
  return meets(a, b) || meets(b, a);
}

Enchanted Enchant(const Weapon& weapon, const std::vector<const Enchantment*>& enchantments,
                  const GearEnchantments& gear) {
  Enchanted out{weapon, {}, 1, {}};
  Weapon& w = out.weapon;
  // Effects that work the same on any item.
  const auto shared = [&](const Mutator& m) {
    if (m.tag == "AddOnPlayerShootActivate") {
      Activate effect = m.attributes;
      effect.type = m.text;
      w.on_shoot.push_back(std::move(effect));
      return true;
    }
    return AddStatMutator(m, out.stats);
  };
  for (const auto& [slot, list] : gear) {
    for (const Enchantment* e : list) {
      bool modelled = true;
      for (const Mutator& m : e->mutators) {
        if (slot == "ABILITY" && m.tag == "MultiplyMPCost") {
          out.mp_cost *= m.number.value_or(1);
        } else if (!shared(m)) {
          modelled = false;
        }
      }
      if (!modelled) {
        std::string item = slot;
        std::ranges::transform(item, item.begin(), [](unsigned char ch) {
          return static_cast<char>(std::tolower(ch));
        });
        out.unsupported.push_back(e->name + " (" + item + " enchantment)");
      }
    }
  }
  // Awakened subattacks go in before any multiplier, so a fire rate or
  // projectile bonus reaches them whatever order the enchantments come in.
  std::vector<const Enchantment*> failed;
  for (const Enchantment* e : enchantments) {
    for (const Mutator& m : e->mutators) {
      if ((m.tag == "SetSubAttack" || m.tag == "AddSubAttack") && !TakeSubattack(w, *e, m) &&
          std::ranges::find(failed, e) == failed.end()) {
        failed.push_back(e);
      }
    }
  }
  for (const Enchantment* e : enchantments) {
    bool modelled = std::ranges::find(failed, e) == failed.end();
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
      } else if (!shared(m)) {
        modelled = false;
      }
    }
    if (!modelled) {
      out.unsupported.push_back(e->name + " (enchantment)");
    }
  }
  return out;
}

}  // namespace rotmg
