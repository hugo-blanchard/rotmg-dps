#include "score.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>
#include <utility>

#include "rotmg/optimize.hpp"

namespace rotmg::search {

namespace {

constexpr std::array<int Character::*, kStats> kFields = {
    &Character::attack,   &Character::dexterity, &Character::speed,          &Character::wisdom,
    &Character::vitality, &Character::defense,   &Character::max_hit_points, &Character::max_magic_points};

// The items' order when the engine collects enchantment stats: the gear's,
// then the weapon's.
constexpr std::array<std::size_t, kSlots> kCollectOrder = {kRing, kArmor, kAbility, kWeapon};

void AddBonuses(Stats& to, const std::vector<StatBonus>& bonuses) {
  for (const StatBonus& b : bonuses) {
    if (const int s = StatIndex(b.stat); s >= 0) {
      to[static_cast<std::size_t>(s)] += b.amount;
    }
  }
}

void AddOwn(std::vector<Conversion>& to, const std::vector<Activate>& on_equip) {
  for (const Activate& a : on_equip) {
    const bool of_total = a.type == "IncrementStatRelative";
    if (!of_total && a.type != "BonusStatRelative") {
      continue;
    }
    const int stat = StatIndex(a.Text("stat"));
    const int from = StatIndex(a.Text("statRelativeTo"));
    if (stat >= 0 && from >= 0) {
      to.push_back({static_cast<std::uint8_t>(from), static_cast<std::uint8_t>(stat),
                    a.Number("amount").value_or(0), of_total});
    }
  }
}

// RealmEye, Item Stat-Scaling: relative bonuses all come from the stats
// before any of them, each stat's total rounds down. Bonus stats are those
// above the class max, as on a maxed character; without a class only
// shares of the whole stat count.
Stats Gains(const std::vector<Conversion>& conversions, const Stats& everything, const Stats& class_max,
            bool has_class) {
  std::array<double, kStats> sum{};
  for (const Conversion& c : conversions) {
    if (!c.of_total && !has_class) {
      continue;
    }
    const int of = c.of_total ? everything[c.from] : std::max(0, everything[c.from] - class_max[c.from]);
    sum[c.to] += c.percent / 100 * of;
  }
  Stats out{};
  for (std::size_t s = 0; s < kStats; ++s) {
    out[s] = static_cast<int>(std::floor(sum[s] + 1e-9));
  }
  return out;
}

}  // namespace

Layout LayoutOf(const Model& model, const Build& build) {
  Layout out;
  out.items = build.items;
  for (std::size_t s = 0; s < kSlots; ++s) {
    for (const std::int32_t e : build.enchants[s]) {
      if (!model.effect(e).stats_only) {
        out.specials[s].push_back(e);
      }
    }
    std::ranges::sort(out.specials[s]);
  }
  return out;
}

EnchantStats StatsOf(const Model& model, const Build& build) {
  EnchantStats out;
  for (const std::size_t s : kCollectOrder) {
    for (const std::int32_t e : build.enchants[s]) {
      const StatEffect& effect = model.effect(e);
      for (std::size_t k = 0; k < kStats; ++k) {
        out.tenths[k] += effect.tenths[k];
      }
      out.conversions.insert(out.conversions.end(), effect.conversions.begin(), effect.conversions.end());
    }
  }
  return out;
}

const PreparedLayout* Scorer::Prepare(const Layout& layout) {
  if (auto it = prepared_.find(layout); it != prepared_.end()) {
    return it->second.get();
  }
  std::unique_ptr<PreparedLayout> out;
  if (layout.items[kWeapon] >= 0) {
    RequestSpec spec = model_->base();
    const auto named = [&](std::size_t slot) {
      return layout.items[slot] < 0 ? std::nullopt : std::optional<std::string>(model_->ItemName(slot, layout.items[slot]));
    };
    const auto ids = [&](std::size_t slot) {
      std::vector<std::string> out_ids;
      for (const std::int32_t e : layout.specials[slot]) {
        out_ids.push_back(model_->enchantment(e).id);
      }
      return out_ids;
    };
    spec.weapon = model_->ItemName(kWeapon, layout.items[kWeapon]);
    spec.ability = named(kAbility);
    spec.armor = named(kArmor);
    spec.ring = named(kRing);
    spec.enchantments = ids(kWeapon);
    spec.gear_enchantments.emplace();
    (*spec.gear_enchantments)["ability"] = ids(kAbility);
    (*spec.gear_enchantments)["armor"] = ids(kArmor);
    (*spec.gear_enchantments)["ring"] = ids(kRing);
    try {
      Prepared p = rotmg::Prepare(spec, model_->dataset());
      if (p.request.weapon != nullptr && !p.request.unknown_ability) {
        out = std::make_unique<PreparedLayout>();
        Request& r = out->request;
        r = std::move(p.request);
        // The layout's enchantment stats are worked out with the build's
        // others, so they come off; the base stats go on, as Finish does.
        Character& c = r.character;
        for (const StatBonus& b : r.enchantment_stats) {
          if (const int s = StatIndex(b.stat); s >= 0) {
            c.*kFields[static_cast<std::size_t>(s)] -= b.amount;
          }
        }
        r.enchantment_stats.clear();
        for (std::size_t s = 0; s < kStats; ++s) {
          c.*kFields[s] += spec.character.*kFields[s];
        }
        c.status = spec.character.status;
        AddBonuses(out->item_bonuses, r.weapon->stat_bonuses);
        if (r.ability != nullptr) {
          AddBonuses(out->item_bonuses, r.ability->stat_bonuses);
          AddOwn(out->own, r.ability->on_equip);
        }
        AddOwn(out->own, r.weapon->on_equip);
        for (const Gear* g : {r.armor, r.ring}) {
          if (g != nullptr) {
            AddOwn(out->own, g->on_equip);
          }
        }
      }
    } catch (const std::exception&) {
      // A layout the engine rejects, like an enchantment that doesn't fit,
      // has no score.
    }
  }
  return prepared_.emplace(layout, std::move(out)).first->second.get();
}

Character Scorer::Final(const PreparedLayout& layout, const EnchantStats& stats) const {
  Character c = layout.request.character;
  for (std::size_t s = 0; s < kStats; ++s) {
    const std::int32_t t = stats.tenths[s];
    // Rounded down, negative sums too.
    c.*kFields[s] += t >= 0 ? t / 10 : -((-t + 9) / 10);
  }
  if (stats.conversions.empty() && layout.own.empty()) {
    return c;
  }
  Stats everything{};
  for (std::size_t s = 0; s < kStats; ++s) {
    everything[s] = c.*kFields[s] + layout.item_bonuses[s];
  }
  const bool has_class = model_->class_max() != Stats{};
  const Stats gains = Gains(stats.conversions, everything, model_->class_max(), has_class);
  const Stats own = Gains(layout.own, everything, model_->class_max(), has_class);
  for (std::size_t s = 0; s < kStats; ++s) {
    c.*kFields[s] += gains[s] + own[s];
  }
  return c;
}

double Scorer::Score(const PreparedLayout& layout, const Character& character) const {
  const Request& r = layout.request;
  const BuildNumbers e = EvaluateBuildNumbers(*r.weapon, r.ability, character, r.target, r.assumptions);
  constexpr double kTieBreak = 1e-6;
  return weights_.weapon * e.weapon_dps + weights_.ability * e.ability_dps + kTieBreak * e.dps;
}

}  // namespace rotmg::search
