#include "bounded.hpp"

#include <algorithm>
#include <utility>

namespace rotmg::search {

std::optional<EnchantResult> BestBoundedBuild(const Model& model, Weights weights, std::int32_t weapon,
                                              std::int32_t ability, const BoundedSettings& settings) {
  // The pairs, by their score without enchantments, best first: the best
  // build usually comes early, and then bounds every pair after it. Ties
  // keep the order the candidates were given in.
  std::vector<std::pair<double, std::array<std::int32_t, kSlots>>> pairs;
  {
    Scorer scorer(model, weights);
    for (const std::int32_t armor : settings.armors) {
      for (const std::int32_t ring : settings.rings) {
        const std::array<std::int32_t, kSlots> items = {weapon, ability, armor, ring};
        const PreparedLayout* layout = scorer.Prepare(Layout{items, {}});
        if (layout != nullptr) {
          pairs.emplace_back(scorer.Score(*layout, scorer.Final(*layout, EnchantStats{})), items);
        }
      }
    }
  }
  std::ranges::stable_sort(pairs, [](const auto& a, const auto& b) { return a.first > b.first; });

  BoundedProgress progress;
  progress.pairs = pairs.size();
  const auto report = [&](double within) {
    if (settings.progress) {
      progress.within = within;
      settings.progress(progress);
    }
  };
  EnchantSettings enchant = settings.enchant;
  enchant.progress = report;
  double best = -1;
  for (const auto& [plain, items] : pairs) {
    report(0);
    // A scorer per pair: it keeps every layout it prepares, which over
    // thousands of pairs would hold gigabytes.
    Scorer scorer(model, weights);
    if (std::optional<EnchantResult> found = BestEnchantments(model, scorer, items, enchant, best);
        found && found->score > best) {
      best = found->score;
      progress.best = std::move(found);
    }
    ++progress.done;
  }
  report(0);
  return progress.best;
}

}  // namespace rotmg::search
