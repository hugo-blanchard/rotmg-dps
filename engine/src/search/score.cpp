#include "score.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

namespace rotmg::search {

namespace {

// The items' order when the engine collects enchantment stats: the gear's,
// then the weapon's.
constexpr std::array<std::size_t, kSlots> kCollectOrder = {kRing, kArmor, kAbility, kWeapon};

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
        out->stats = StatBaseOf(p, spec.character);
        Request& r = out->request;
        r = std::move(p.request);
        r.character = rotmg::WithStats(r.character, out->stats.before);
        r.character.status = spec.character.status;
      }
    } catch (const std::exception&) {
      // A layout the engine rejects, like an enchantment that doesn't fit,
      // has no score.
    }
  }
  return prepared_.emplace(layout, std::move(out)).first->second.get();
}

Character Scorer::Final(const PreparedLayout& layout, const EnchantStats& stats) const {
  return Final(layout, stats.tenths, stats.conversions);
}

Character Scorer::Final(const PreparedLayout& layout, const Tenths& tenths,
                        std::span<const Conversion> conversions) const {
  return WithStats(layout, FinalStats(layout, tenths, conversions));
}

Character Scorer::WithStats(const PreparedLayout& layout, const Stats& stats) const {
  return rotmg::WithStats(layout.request.character, stats);
}

double Scorer::Score(const PreparedLayout& layout, const Character& character) const {
  const Request& r = layout.request;
  const BuildNumbers e = EvaluateBuildNumbers(*r.weapon, r.ability, character, r.target, r.assumptions, weights_);
  constexpr double kTieBreak = 1e-6;
  return weights_.weapon * e.weapon_dps + weights_.ability * e.ability_dps + kTieBreak * e.dps;
}

double Scorer::Weighted(const PreparedLayout& layout, const Character& character) const {
  const Request& r = layout.request;
  const BuildNumbers e = EvaluateBuildNumbers(*r.weapon, r.ability, character, r.target, r.assumptions, weights_);
  return weights_.weapon * e.weapon_dps + weights_.ability * e.ability_dps;
}

}  // namespace rotmg::search
