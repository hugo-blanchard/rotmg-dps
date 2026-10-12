#include "rotmg/optimize.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "parallel.hpp"
#include "rotmg/dataset.hpp"
#include "rotmg/enchant.hpp"

namespace rotmg {
namespace {

constexpr std::size_t kSlotCount = 4;
// How much of a stat is added to see whether a build's DPS depends on it:
// enough to cross the steps some effects scale in, like one more target
// every 10 WIS, and then much more in case nothing scales until a stat is
// well above where the build has it (an ability's statModScalingMin).
constexpr std::array<int, 2> kProbes = {20, 80};

using Build = OptimizerBuild;
using Mode = OptimizerProblem::Enchantments;
using Stats = std::array<int, 8>;
using Mask = std::array<bool, 8>;
using Rates = std::array<double, 8>;

int StatIndex(std::string_view stat) {
  for (std::size_t i = 0; i < kOptimizerStats.size(); ++i) {
    if (stat == kOptimizerStats[i]) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool Better(double a, double b) { return a > b * (1 + 1e-9); }

// What an enchantment does to stats. One that only adds stats (flat or as a
// share of another stat), or only boosts a stat when you shoot, matters to
// a build exactly when one of those stats does.
struct StatEffect {
  bool by_stats = true;
  // Only flat stats: one can be compared with another stat by stat.
  bool flat_only = true;
  std::array<double, 8> flat{};
  Mask raises{};
  // Its labels (without tiers) and the labels it can't go with: two
  // enchantments with the same conflict with the same others.
  std::string conflicts;
};

StatEffect EffectOf(const Enchantment& e) {
  StatEffect out;
  out.by_stats = !e.mutators.empty();
  for (const Mutator& m : e.mutators) {
    const int stat = StatIndex(m.attributes.Text("stat"));
    const double amount = m.attributes.Number("amount").value_or(0);
    const bool equip = m.tag == "ActivateOnEquip";
    if (equip && m.text == "IncrementStat" && stat >= 0) {
      out.flat[static_cast<std::size_t>(stat)] += amount;
    } else if (((equip && m.text == "BonusStatRelative") ||
                (m.tag == "AddOnPlayerShootActivate" && m.text == "StatBoostSelf")) &&
               stat >= 0) {
      out.flat_only = false;
    } else {
      out.by_stats = false;
      out.flat_only = false;
      continue;
    }
    if (amount > 0) {
      out.raises[static_cast<std::size_t>(stat)] = true;
    }
  }
  std::vector<std::string> labels;
  for (const std::string& l : e.labels) {
    if (!(l.starts_with("TIER") && l.size() == 5)) {
      labels.push_back(l);
    }
  }
  std::vector<std::string> against = e.incompatible_labels;
  std::ranges::sort(labels);
  std::ranges::sort(against);
  for (const std::string& l : labels) out.conflicts += l + ",";
  out.conflicts += "|";
  for (const std::string& l : against) out.conflicts += l + ",";
  return out;
}

class BuildSearch {
 public:
  BuildSearch(const Dataset& d, const OptimizerProblem& p) : d_(d), p_(p) {
    for (const Enchantment& e : d_.enchantments) {
      effects_.emplace(&e, EffectOf(e));
    }
    const auto count = [&](const std::vector<std::string>& labels) {
      for (const std::string& l : labels) ++carriers_[l];
    };
    for (const Weapon& w : d_.weapons) count(w.labels);
    for (const Ability& a : d_.abilities) count(a.labels);
    for (const Gear& g : d_.armors) count(g.labels);
    for (const Gear& g : d_.rings) count(g.labels);
  }

  std::optional<OptimizerResult> Run();

  // Thrown on a thread that stops because the search is stopping.
  struct Stopped {};

 private:
  // What can go on the item in a slot, by the game's rules, and how many.
  struct Fitting {
    std::vector<const Enchantment*> options;
    int slots = 0;
  };

  const Fitting& Fits(std::size_t slot, const std::string& item);
  void Adapt(Build& b, std::size_t slot);
  std::optional<double> Score(const Build& b, const Stats& extra = {});
  std::vector<std::optional<double>> ScoreAll(const std::vector<Build>& builds, const Stats& extra = {});
  Rates StatRates(const Build& b);
  Mask Sensitive(const Build& b);
  std::vector<const Enchantment*> Relevant(const Build& b, std::size_t slot);
  std::vector<const Enchantment*> Shortlist(Build& b, std::size_t slot);
  double Fill(Build& b, std::size_t slot);
  std::pair<double, Build> LinearFill(Build b);
  double Exchange(Build& b);
  double FillAll(Build& b);
  std::vector<const Enchantment*> OwnEnchantments(std::size_t slot, const std::string& item);
  std::pair<double, Build> Descend(Build current, std::size_t index);
  std::vector<std::string> Seeds();
  std::optional<OptimizerResult> Search();
  OptimizerResult Results();
  void Report();
  static std::string Key(const Build& b);

  const Dataset& d_;
  const OptimizerProblem& p_;
  std::map<const Enchantment*, StatEffect> effects_;
  std::map<std::pair<std::size_t, std::string>, Fitting> fitting_;
  // How many items carry each label.
  std::map<std::string, int> carriers_;
  std::unordered_map<std::string, std::optional<double>> scores_;
  std::unordered_map<std::string, Rates> rates_;
  std::map<std::string, std::pair<double, Build>> seen_;
  // Whether an enchantment that does more than add stats changes an
  // item's DPS, by slot, item and enchantment.
  std::map<std::tuple<std::size_t, std::string, const Enchantment*>, bool> tested_;
  // Relevant's answers, by slot, item and which stats matter.
  std::map<std::tuple<std::size_t, std::string, unsigned>, std::vector<const Enchantment*>> relevant_;
  // Guards the caches above, which the search's threads share. Each is
  // looked up and filled under it, and worked out outside it; two threads
  // may work out the same thing, which only costs time.
  std::mutex mu_;
  // Set once a progress report throws, like a caller cutting the search
  // short: every thread then stops at its next build, and the search
  // rethrows what the report threw.
  std::atomic<bool> stopping_ = false;
  std::exception_ptr stopped_by_;
  double baseline_ = 0;
  // For progress reports: how far along the search is, and the share of it
  // the current step started at and takes.
  // For progress reports, under mu_: how far along each descent is, from 0
  // to 1, and the share of the whole search each takes, the trading of
  // enchantments at the end being the last.
  std::vector<double> parts_done_;
  std::vector<double> part_sizes_;
  std::chrono::steady_clock::time_point last_report_ = std::chrono::steady_clock::now();
};

std::string BuildSearch::Key(const Build& b) {
  std::string key;
  for (std::size_t i = 0; i < kSlotCount; ++i) {
    key += b.items[i] + "\n";
    for (const std::string& id : b.enchants[i]) {
      key += id + ",";
    }
    key += "\n";
  }
  return key;
}

const BuildSearch::Fitting& BuildSearch::Fits(std::size_t slot, const std::string& item) {
  const auto key = std::pair{slot, item};
  {
    std::lock_guard lock(mu_);
    if (auto it = fitting_.find(key); it != fitting_.end()) {
      return it->second;
    }
  }
  const Weapon* w = slot == 0 ? FindWeapon(d_, item) : nullptr;
  const Ability* a = slot == 1 ? FindAbility(d_, item) : nullptr;
  const Gear* g = slot == 2 ? FindGear(d_.armors, item) : slot == 3 ? FindGear(d_.rings, item) : nullptr;
  Fitting out;
  out.slots = w   ? EnchantmentSlots(w->enchanting, d_)
              : a ? EnchantmentSlots(a->enchanting, d_)
              : g ? EnchantmentSlots(g->enchanting, d_)
                  : slot == 1 ? 4 : 0;
  for (const Enchantment& e : d_.enchantments) {
    const Obtained how = w   ? HowObtained(e, *w, d_)
                         : a ? HowObtained(e, *a, d_)
                         : g ? HowObtained(e, *g, d_)
                             : slot == 1 ? HowObtainedOnSlot(e, "ABILITY") : Obtained::kNever;
    if (how == Obtained::kRolls || (p_.artifacts && how == Obtained::kArtifact)) {
      out.options.push_back(&e);
    }
  }
  std::lock_guard lock(mu_);
  return fitting_.try_emplace(key, std::move(out)).first->second;
}

// Keeps the enchantments that still fit the slot's item, in order, as many
// as it holds.
void BuildSearch::Adapt(Build& b, std::size_t slot) {
  if (p_.enchantments == Mode::kNone) {
    b.enchants[slot].clear();
    return;
  }
  const Fitting& fits = Fits(slot, b.items[slot]);
  std::erase_if(b.enchants[slot], [&](const std::string& id) {
    return std::ranges::none_of(fits.options, [&](const Enchantment* e) { return e->id == id; });
  });
  if (std::cmp_greater(b.enchants[slot].size(), fits.slots)) {
    b.enchants[slot].resize(static_cast<std::size_t>(fits.slots));
  }
}

std::optional<double> BuildSearch::Score(const Build& b, const Stats& extra) {
  if (stopping_) {
    throw Stopped{};
  }
  const bool plain = std::ranges::all_of(extra, [](int x) { return x == 0; });
  std::string key = Key(b);
  if (!plain) {
    for (int x : extra) key += std::to_string(x) + ",";
  }
  {
    std::lock_guard lock(mu_);
    if (auto it = scores_.find(key); it != scores_.end()) {
      return it->second;
    }
  }
  // Only the thread that started the search can call back into the caller,
  // so it checks whether a report is due on each build it scores.
  if (p_.progress && !OnHelperThread()) {
    Report();
  }
  const std::optional<double> dps = p_.score(b, extra);
  std::lock_guard lock(mu_);
  scores_.emplace(key, dps);
  if (!plain || !dps) {
    return dps;
  }
  // One entry per set of items, with its best enchantments. Of two that
  // score the same, the first by key stays, whichever thread got there
  // first, so the result doesn't depend on the order.
  std::string items_key;
  for (const std::string& item : b.items) {
    items_key += item + "\n";
  }
  auto it = seen_.find(items_key);
  if (it == seen_.end() || it->second.first < *dps || (it->second.first == *dps && key < Key(it->second.second))) {
    seen_[items_key] = {*dps, b};
  }
  return dps;
}

// Scores builds that don't depend on each other at once, spread over the
// engine's threads. What the search does with them stays in order, so a
// search finds the same builds however many threads it has.
std::vector<std::optional<double>> BuildSearch::ScoreAll(const std::vector<Build>& builds, const Stats& extra) {
  std::vector<std::optional<double>> out(builds.size());
  ParallelFor(builds.size(), [&](std::size_t i) { out[i] = Score(builds[i], extra); });
  return out;
}

// How much DPS a point of each stat adds to the build, over the smallest
// probe that shows any.
Rates BuildSearch::StatRates(const Build& b) {
  const std::string key = Key(b);
  {
    std::lock_guard lock(mu_);
    if (auto it = rates_.find(key); it != rates_.end()) {
      return it->second;
    }
  }
  Rates out{};
  const double base = Score(b).value_or(0);
  std::array<bool, 8> found{};
  for (const int probe : kProbes) {
    std::vector<std::optional<double>> probed(out.size());
    ParallelFor(out.size(), [&](std::size_t i) {
      if (!found[i]) {
        Stats extra{};
        extra[i] = probe;
        probed[i] = Score(b, extra);
      }
    });
    for (std::size_t i = 0; i < out.size(); ++i) {
      if (!found[i] && probed[i] && Better(*probed[i], base)) {
        out[i] = (*probed[i] - base) / probe;
        found[i] = true;
      }
    }
  }
  std::lock_guard lock(mu_);
  return rates_.try_emplace(key, out).first->second;
}

// The stats that would raise the build's DPS.
Mask BuildSearch::Sensitive(const Build& b) {
  const Rates rates = StatRates(b);
  Mask out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = rates[i] > 0;
  }
  return out;
}

// The enchantments worth trying on the slot's item in this build. One that
// only adds stats counts when it raises a stat the build's DPS depends on,
// so +DEF counts once something scales with DEF. Of those that only add
// flat stats and clash with the same others, one that adds at least as
// much of every stat that matters makes the other pointless (Attack Bonus
// IV over III). Anything else counts when it changes the DPS on its own.
std::vector<const Enchantment*> BuildSearch::Relevant(const Build& b, std::size_t slot) {
  Build bare = b;
  bare.enchants[slot].clear();
  const Mask matters = Sensitive(bare);
  unsigned bits = 0;
  for (std::size_t i = 0; i < matters.size(); ++i) {
    bits |= matters[i] ? 1u << i : 0u;
  }
  const auto cache_key = std::tuple{slot, b.items[slot], bits};
  {
    std::lock_guard lock(mu_);
    if (auto it = relevant_.find(cache_key); it != relevant_.end()) {
      return it->second;
    }
  }
  const double without = Score(bare).value_or(0);
  // Enchantments that do more than add stats, each on its own, at once.
  {
    std::vector<const Enchantment*> untested;
    const Fitting& fits = Fits(slot, b.items[slot]);
    {
      std::lock_guard lock(mu_);
      for (const Enchantment* e : fits.options) {
        if (!effects_.at(e).by_stats && !tested_.contains(std::tuple{slot, b.items[slot], e})) {
          untested.push_back(e);
        }
      }
    }
    std::vector<Build> ones(untested.size(), bare);
    for (std::size_t i = 0; i < untested.size(); ++i) {
      ones[i].enchants[slot] = {untested[i]->id};
    }
    const std::vector<std::optional<double>> scored = ScoreAll(ones);
    std::lock_guard lock(mu_);
    for (std::size_t i = 0; i < untested.size(); ++i) {
      const auto& s = scored[i];
      tested_.try_emplace(std::tuple{slot, b.items[slot], untested[i]}, s && std::abs(*s - without) > without * 1e-9);
    }
  }
  std::vector<const Enchantment*> out;
  for (const Enchantment* e : Fits(slot, b.items[slot]).options) {
    const StatEffect& effect = effects_.at(e);
    if (effect.by_stats) {
      bool raises = false;
      for (std::size_t i = 0; i < matters.size(); ++i) {
        raises = raises || (matters[i] && effect.raises[i]);
      }
      if (raises) {
        out.push_back(e);
      }
      continue;
    }
    const auto key = std::tuple{slot, b.items[slot], e};
    std::optional<bool> changes;
    {
      std::lock_guard lock(mu_);
      if (auto it = tested_.find(key); it != tested_.end()) {
        changes = it->second;
      }
    }
    if (!changes) {
      Build one = bare;
      one.enchants[slot] = {e->id};
      const auto s = Score(one);
      changes = s && std::abs(*s - without) > without * 1e-9;
      std::lock_guard lock(mu_);
      tested_.try_emplace(key, *changes);
    }
    if (*changes) {
      out.push_back(e);
    }
  }
  const auto covers = [&](const Enchantment* a, const Enchantment* x) {
    const StatEffect& ea = effects_.at(a);
    const StatEffect& ex = effects_.at(x);
    if (!ea.flat_only || !ex.flat_only || ea.conflicts != ex.conflicts) {
      return false;
    }
    bool more = false;
    for (std::size_t i = 0; i < matters.size(); ++i) {
      if (!matters[i]) continue;
      if (ea.flat[i] < ex.flat[i]) return false;
      more = more || ea.flat[i] > ex.flat[i];
    }
    return more || a->id < x->id;
  };
  // Only enchantments that clash with the same others are compared.
  std::map<std::string, std::vector<const Enchantment*>> groups;
  for (const Enchantment* e : out) {
    groups[effects_.at(e).conflicts].push_back(e);
  }
  std::vector<const Enchantment*> kept;
  for (const Enchantment* x : out) {
    const auto& group = groups[effects_.at(x).conflicts];
    if (std::ranges::none_of(group, [&](const Enchantment* a) { return a != x && covers(a, x); })) {
      kept.push_back(x);
    }
  }
  std::lock_guard lock(mu_);
  return relevant_.try_emplace(cache_key, std::move(kept)).first->second;
}

// The enchantments to combine on the slot's item, with b's slot left
// empty. Each counts on its own first and only the best are kept. The best
// of every group that clashes with the same others always makes it, or the
// list could hold only enchantments that can't share an item. One that
// only adds flat stats is valued by what those stats are worth here,
// without evaluating it.
std::vector<const Enchantment*> BuildSearch::Shortlist(Build& b, std::size_t slot) {
  b.enchants[slot].clear();
  const double bare = Score(b).value_or(0);
  std::vector<const Enchantment*> options = Relevant(b, slot);
  const std::size_t extra = p_.thorough ? 24 : 12;
  const Rates rates = StatRates(b);
  std::vector<Build> ones;
  for (const Enchantment* e : options) {
    if (!effects_.at(e).flat_only) {
      ones.push_back(b);
      ones.back().enchants[slot] = {e->id};
    }
  }
  const std::vector<std::optional<double>> scored = ScoreAll(ones);
  std::vector<std::pair<double, const Enchantment*>> alone;
  std::size_t next_scored = 0;
  for (const Enchantment* e : options) {
    const StatEffect& effect = effects_.at(e);
    if (effect.flat_only) {
      double gain = 0;
      for (std::size_t i = 0; i < rates.size(); ++i) {
        gain += rates[i] * effect.flat[i];
      }
      alone.emplace_back(bare + gain, e);
      continue;
    }
    alone.emplace_back(scored[next_scored++].value_or(0), e);
  }
  std::ranges::stable_sort(alone, [](const auto& x, const auto& y) { return x.first > y.first; });
  options.clear();
  std::set<std::string> groups;
  std::size_t others = 0;
  for (const auto& [dps, e] : alone) {
    if (groups.insert(effects_.at(e).conflicts).second || others++ < extra) {
      options.push_back(e);
    }
  }
  b.enchants[slot].clear();
  return options;
}

// Fills the slot's item with the set of enchantments that adds the most. A
// beam keeps the best few sets of each size and grows each by one, so a
// pair that only works together is still found.
double BuildSearch::Fill(Build& b, std::size_t slot) {
  b.enchants[slot].clear();
  const double bare = Score(b).value_or(0);
  if (p_.enchantments != Mode::kBest || b.items[slot].empty()) {
    return bare;
  }
  const int size = std::min(p_.per_item, Fits(slot, b.items[slot]).slots);
  if (size <= 0) {
    return bare;
  }
  const std::vector<const Enchantment*> options = Shortlist(b, slot);
  const std::size_t width = p_.thorough ? 20 : 12;
  using Set = std::vector<const Enchantment*>;
  std::vector<std::pair<double, Set>> level = {{bare, {}}};
  std::pair<double, Set> best = {bare, {}};
  for (int depth = 0; depth < size; ++depth) {
    std::set<std::vector<std::string>> made;
    std::vector<Set> grown_sets;
    std::vector<Build> grown_builds;
    for (const auto& [dps, set] : level) {
      for (const Enchantment* e : options) {
        const bool clash =
            std::ranges::any_of(set, [&](const Enchantment* o) { return o == e || Conflict(*o, *e); });
        if (clash) {
          continue;
        }
        Set grown = set;
        grown.push_back(e);
        std::vector<std::string> ids;
        for (const Enchantment* x : grown) ids.push_back(x->id);
        std::ranges::sort(ids);
        if (!made.insert(ids).second) {
          continue;
        }
        grown_builds.push_back(b);
        grown_builds.back().enchants[slot] = ids;
        grown_sets.push_back(std::move(grown));
      }
    }
    const std::vector<std::optional<double>> scored = ScoreAll(grown_builds);
    std::vector<std::pair<double, Set>> next;
    for (std::size_t i = 0; i < scored.size(); ++i) {
      if (scored[i]) {
        next.emplace_back(*scored[i], std::move(grown_sets[i]));
      }
    }
    if (next.empty()) {
      break;
    }
    std::ranges::stable_sort(next, [](const auto& x, const auto& y) { return x.first > y.first; });
    if (next.size() > width) {
      next.resize(width);
    }
    if (Better(next.front().first, best.first)) {
      best = next.front();
    }
    level = std::move(next);
  }
  b.enchants[slot].clear();
  for (const Enchantment* e : best.second) {
    b.enchants[slot].push_back(e->id);
  }
  std::ranges::sort(b.enchants[slot]);
  return best.first;
}

// Swaps enchantments on the finished build: any one for another that fits,
// then two at once on different items, which finds trades a single swap
// can't, like giving up Pirates Expertise on two items for Shaitan's Might
// on one and Attack To Dexterity on the other.
double BuildSearch::Exchange(Build& b) {
  double best = Score(b).value_or(0);
  if (p_.enchantments != Mode::kBest) {
    return best;
  }
  // Every way to change one item's enchantments by one: replace one with
  // another from its shortlist, add one if there's room, or drop one.
  const auto changes = [&](const Build& from, std::size_t slot) {
    std::vector<std::vector<std::string>> out;
    if (from.items[slot].empty()) {
      return out;
    }
    Build around = from;
    const std::vector<const Enchantment*> options = Shortlist(around, slot);
    const auto& now = from.enchants[slot];
    const int room = std::min(p_.per_item, Fits(slot, from.items[slot]).slots);
    const auto fits_with = [&](const Enchantment* e, const std::vector<std::string>& others) {
      return std::ranges::none_of(others, [&](const std::string& id) {
        const auto o = std::ranges::find(d_.enchantments, id, &Enchantment::id);
        return id == e->id || (o != d_.enchantments.end() && Conflict(*o, *e));
      });
    };
    for (std::size_t k = 0; k <= now.size(); ++k) {
      std::vector<std::string> rest = now;
      if (k < now.size()) {
        rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(k));
        out.push_back(rest);
      } else if (std::cmp_greater_equal(now.size(), room)) {
        continue;
      }
      for (const Enchantment* e : options) {
        if (fits_with(e, rest)) {
          std::vector<std::string> next = rest;
          next.push_back(e->id);
          std::ranges::stable_sort(next);
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  };
  for (int pass = 0; pass < 4; ++pass) {
    std::array<std::vector<std::vector<std::string>>, kSlotCount> moves;
    for (std::size_t i = 0; i < kSlotCount; ++i) {
      moves[i] = changes(b, i);
    }
    Build next = b;
    double next_dps = best;
    // Each item's single changes, best first; only the best few are paired.
    constexpr std::size_t kPaired = 15;
    for (std::size_t i = 0; i < kSlotCount; ++i) {
      std::vector<Build> tried(moves[i].size(), b);
      for (std::size_t k = 0; k < moves[i].size(); ++k) {
        tried[k].enchants[i] = moves[i][k];
      }
      const std::vector<std::optional<double>> scores = ScoreAll(tried);
      std::vector<std::pair<double, std::vector<std::string>>> scored;
      for (std::size_t k = 0; k < tried.size(); ++k) {
        if (const auto& s = scores[k]) {
          scored.emplace_back(*s, moves[i][k]);
          if (Better(*s, next_dps)) {
            next_dps = *s;
            next = tried[k];
          }
        }
      }
      std::ranges::stable_sort(scored, [](const auto& x, const auto& y) { return x.first > y.first; });
      moves[i].clear();
      for (std::size_t k = 0; k < scored.size() && k < kPaired; ++k) {
        moves[i].push_back(scored[k].second);
      }
    }
    if (!Better(next_dps, best)) {
      for (std::size_t i = 0; i < kSlotCount; ++i) {
        for (std::size_t j = i + 1; j < kSlotCount; ++j) {
          std::vector<Build> tried;
          for (const auto& mi : moves[i]) {
            for (const auto& mj : moves[j]) {
              tried.push_back(b);
              tried.back().enchants[i] = mi;
              tried.back().enchants[j] = mj;
            }
          }
          const std::vector<std::optional<double>> scores = ScoreAll(tried);
          for (std::size_t k = 0; k < tried.size(); ++k) {
            if (scores[k] && Better(*scores[k], next_dps)) {
              next_dps = *scores[k];
              next = tried[k];
            }
          }
        }
      }
    }
    if (!Better(next_dps, best)) {
      break;
    }
    b = next;
    best = next_dps;
  }
  return best;
}

// Fills every slot in turn, again while that still adds something, since
// one item's enchantments can make another's worth more.
double BuildSearch::FillAll(Build& b) {
  double dps = Score(b).value_or(0);
  if (p_.enchantments != Mode::kBest) {
    return dps;
  }
  for (int pass = 0; pass < 3; ++pass) {
    const double before = dps;
    for (std::size_t i = 0; i < kSlotCount; ++i) {
      dps = Fill(b, i);
    }
    if (!Better(dps, before)) {
      break;
    }
  }
  if (auto linear = LinearFill(b); Better(linear.first, dps)) {
    b = std::move(linear.second);
    dps = linear.first;
    for (std::size_t i = 0; i < kSlotCount; ++i) {
      dps = Fill(b, i);
    }
  }
  return dps;
}

// Fills every item at once by what each stat is worth to the build, as the
// probes measure it, rather than by what each step adds. When nothing
// scales with a stat until it's well above where the build has it, every
// single enchantment of it looks worthless, yet stacking it on every item
// can make the best build. Enchantments that do more than add flat stats
// are valued by what they add on their own. The result is checked with the
// real DPS, then filled again with the rates of the build it made while
// that changes anything.
std::pair<double, Build> BuildSearch::LinearFill(Build b) {
  for (auto& list : b.enchants) {
    list.clear();
  }
  double best_dps = Score(b).value_or(0);
  Build best = b;
  Build context = b;
  for (int round = 0; round < 3; ++round) {
    const Rates rates = StatRates(context);
    Build next = b;
    for (std::size_t slot = 0; slot < kSlotCount; ++slot) {
      if (next.items[slot].empty()) {
        continue;
      }
      const int room = std::min(p_.per_item, Fits(slot, next.items[slot]).slots);
      Build around = context;
      around.enchants[slot].clear();
      const double without = Score(around).value_or(0);
      const std::vector<const Enchantment*> relevant = Relevant(around, slot);
      std::vector<Build> ones;
      for (const Enchantment* e : relevant) {
        if (!effects_.at(e).flat_only) {
          ones.push_back(around);
          ones.back().enchants[slot] = {e->id};
        }
      }
      const std::vector<std::optional<double>> scored = ScoreAll(ones);
      std::size_t next_scored = 0;
      std::vector<std::pair<double, const Enchantment*>> valued;
      for (const Enchantment* e : relevant) {
        const StatEffect& effect = effects_.at(e);
        double value = 0;
        if (effect.flat_only) {
          for (std::size_t i = 0; i < rates.size(); ++i) {
            value += rates[i] * effect.flat[i];
          }
        } else {
          value = scored[next_scored++].value_or(0) - without;
        }
        if (value > 0) {
          valued.emplace_back(value, e);
        }
      }
      std::ranges::stable_sort(valued, [](const auto& x, const auto& y) { return x.first > y.first; });
      // The best pair by value, then the best that still fits for any room
      // left; the values add, so this needs no evaluating.
      std::vector<const Enchantment*> chosen;
      double chosen_value = 0;
      const std::size_t top = std::min<std::size_t>(valued.size(), 30);
      for (std::size_t i = 0; i < top && room >= 1; ++i) {
        if (valued[i].first > chosen_value) {
          chosen = {valued[i].second};
          chosen_value = valued[i].first;
        }
        for (std::size_t j = i + 1; j < top && room >= 2; ++j) {
          const double v = valued[i].first + valued[j].first;
          if (v > chosen_value && !Conflict(*valued[i].second, *valued[j].second)) {
            chosen = {valued[i].second, valued[j].second};
            chosen_value = v;
          }
        }
      }
      for (const auto& [value, e] : valued) {
        if (std::cmp_greater_equal(chosen.size(), room)) {
          break;
        }
        if (std::ranges::none_of(chosen, [&](const Enchantment* c) { return c == e || Conflict(*c, *e); })) {
          chosen.push_back(e);
        }
      }
      for (const Enchantment* e : chosen) {
        next.enchants[slot].push_back(e->id);
      }
      std::ranges::stable_sort(next.enchants[slot]);
    }
    if (const auto s = Score(next); s && Better(*s, best_dps)) {
      best_dps = *s;
      best = next;
    }
    if (Key(next) == Key(context)) {
      break;
    }
    context = next;
  }
  return {best_dps, best};
}

// The enchantments made for the item and a few others, like its awakening
// or an engraving, which can make it far better than it looks bare: ones
// that ask for a label at most a handful of items carry.
std::vector<const Enchantment*> BuildSearch::OwnEnchantments(std::size_t slot, const std::string& item) {
  constexpr int kFew = 6;
  std::vector<const Enchantment*> out;
  for (const Enchantment* e : Fits(slot, item).options) {
    const auto few = [&](const std::string& l) {
      const auto it = carriers_.find(l);
      return it == carriers_.end() || it->second <= kFew;
    };
    if (std::ranges::any_of(e->compatible, few)) {
      out.push_back(e);
    }
  }
  return out;
}

// Local search from one build: in turn, every slot gets the item that
// gives the most once enchantments are chosen again around it, and whole
// sets are tried at once, until nothing improves. Candidates are ranked
// first as they are, or with the best of the enchantments made for them,
// then the best few get the full look.
std::pair<double, Build> BuildSearch::Descend(Build current, std::size_t index) {
  const bool best = p_.enchantments == Mode::kBest;
  double current_dps = FillAll(current);
  const std::size_t looks = p_.thorough ? 6 : 3;
  const auto deep = [&](Build b, std::vector<std::size_t> changed) {
    if (!best) {
      return std::pair{Score(b).value_or(0), b};
    }
    // The changed items' enchantments chosen with care, or every item's
    // refilled by what stats are worth now; the move that's taken gets a
    // careful refill of every item afterwards.
    double dps = 0;
    for (std::size_t slot : changed) {
      dps = Fill(b, slot);
    }
    if (auto linear = LinearFill(b); Better(linear.first, dps)) {
      return linear;
    }
    return std::pair{dps, b};
  };
  // For progress reports, the parts of a round in rough units of work, from
  // timing every class: each slot and the sets take about one, the pairs
  // six. A round that improves anything is followed by another of about
  // eight, as most of it is already scored.
  int units = 0;
  const auto at = [&](int rest_of_round, bool improved) {
    const int rest = rest_of_round + (improved ? 8 : 0);
    std::lock_guard lock(mu_);
    parts_done_[index] = static_cast<double>(units) / (units + rest);
  };
  for (int round = 0; round < 8; ++round) {
    bool improved = false;
    const auto take = [&](const std::pair<double, Build>& next) {
      if (Better(next.first, current_dps)) {
        current = next.second;
        current_dps = next.first;
        Build polished = current;
        if (const double dps = FillAll(polished); Better(dps, current_dps)) {
          current = std::move(polished);
          current_dps = dps;
        }
        improved = true;
      }
    };
    // Each slot's candidates, best first, for pairing below.
    std::array<std::vector<std::string>, kSlotCount> ranked;
    for (std::size_t slot = 0; slot < kSlotCount; ++slot) {
      std::vector<Build> nexts;
      for (const std::string& name : p_.candidates[slot]) {
        if (name != current.items[slot]) {
          nexts.push_back(current);
          nexts.back().items[slot] = name;
          Adapt(nexts.back(), slot);
        }
      }
      std::vector<std::optional<double>> scores = ScoreAll(nexts);
      if (best) {
        // Each candidate with each of the enchantments made for it.
        std::vector<std::size_t> of;
        std::vector<Build> withs;
        for (std::size_t k = 0; k < nexts.size(); ++k) {
          if (scores[k]) {
            for (const Enchantment* e : OwnEnchantments(slot, nexts[k].items[slot])) {
              of.push_back(k);
              withs.push_back(nexts[k]);
              withs.back().enchants[slot] = {e->id};
            }
          }
        }
        const std::vector<std::optional<double>> own = ScoreAll(withs);
        for (std::size_t w = 0; w < withs.size(); ++w) {
          if (own[w] && *own[w] > *scores[of[w]]) {
            scores[of[w]] = own[w];
          }
        }
      }
      std::vector<std::pair<double, Build>> tried;
      for (std::size_t k = 0; k < nexts.size(); ++k) {
        if (scores[k]) {
          tried.emplace_back(*scores[k], std::move(nexts[k]));
        }
      }
      std::ranges::stable_sort(tried, [](const auto& a, const auto& b) { return a.first > b.first; });
      for (const auto& [dps, b] : tried) {
        ranked[slot].push_back(b.items[slot]);
      }
      std::vector<std::pair<double, Build>> looked(std::min(looks, tried.size()));
      ParallelFor(looked.size(), [&](std::size_t i) { looked[i] = deep(tried[i].second, {slot}); });
      for (const auto& next : looked) {
        take(next);
      }
      ++units;
      at(static_cast<int>(kSlotCount - slot - 1) + (improved ? 0 : 6) + 1, improved);
    }
    // Two slots at once, when no single change helps: some items only pay
    // off together, like an ability and the ring that feeds what it scales
    // with. The best few of each slot are paired, ranked as they are, and
    // the best pairs get the full look.
    constexpr std::size_t kPairs = 5;
    units += improved ? 0 : 6;
    struct PairLook {
      Build build;
      std::size_t first = 0;
      std::size_t second = 0;
      std::size_t group = 0;
    };
    std::vector<PairLook> to_look;
    std::size_t groups = 0;
    for (std::size_t i = 0; !improved && i < kSlotCount; ++i) {
      for (std::size_t j = i + 1; !improved && j < kSlotCount; ++j) {
        std::vector<Build> nexts;
        for (std::size_t a = 0; a < ranked[i].size() && a < kPairs; ++a) {
          for (std::size_t b = 0; b < ranked[j].size() && b < kPairs; ++b) {
            nexts.push_back(current);
            nexts.back().items[i] = ranked[i][a];
            nexts.back().items[j] = ranked[j][b];
            Adapt(nexts.back(), i);
            Adapt(nexts.back(), j);
          }
        }
        const std::vector<std::optional<double>> scores = ScoreAll(nexts);
        std::vector<std::pair<double, Build>> pairs;
        for (std::size_t k = 0; k < nexts.size(); ++k) {
          if (scores[k]) {
            pairs.emplace_back(*scores[k], std::move(nexts[k]));
          }
        }
        std::ranges::stable_sort(pairs, [](const auto& x, const auto& y) { return x.first > y.first; });
        for (std::size_t k = 0; k < pairs.size() && k < looks; ++k) {
          to_look.push_back({std::move(pairs[k].second), i, j, groups});
        }
        ++groups;
      }
    }
    // Every group's best pairs get their look at once. They're taken in
    // order, and once one improves the later groups go unused, as they
    // always did.
    std::vector<std::pair<double, Build>> pair_looks(to_look.size());
    ParallelFor(to_look.size(), [&](std::size_t n) {
      pair_looks[n] = deep(to_look[n].build, {to_look[n].first, to_look[n].second});
    });
    for (std::size_t n = 0; n < to_look.size(); ++n) {
      if (n > 0 && to_look[n].group != to_look[n - 1].group && improved) {
        break;
      }
      take(pair_looks[n]);
    }
    at(1, improved);
    // Whole sets: ranked as they are, then the best few get the full look.
    std::vector<std::pair<Build, std::size_t>> set_moves;
    for (const EquipmentSet& set : d_.sets) {
      Build next = current;
      int changed = 0;
      std::size_t last = 0;
      for (const EquipmentSet::Piece& piece : set.pieces) {
        const auto slot = static_cast<std::size_t>(piece.slot);
        const auto& options = p_.candidates[slot < kSlotCount ? slot : 0];
        const bool usable = slot < kSlotCount && std::ranges::find(options, piece.name) != options.end();
        if (usable && next.items[slot] != piece.name) {
          next.items[slot] = piece.name;
          Adapt(next, slot);
          ++changed;
          last = slot;
        }
      }
      if (changed >= 2) {
        set_moves.emplace_back(std::move(next), last);
      }
    }
    std::vector<Build> set_builds;
    for (const auto& [next, last] : set_moves) {
      set_builds.push_back(next);
    }
    const std::vector<std::optional<double>> set_scores = ScoreAll(set_builds);
    std::vector<std::tuple<double, Build, std::size_t>> sets;
    for (std::size_t k = 0; k < set_moves.size(); ++k) {
      if (set_scores[k]) {
        sets.emplace_back(*set_scores[k], std::move(set_moves[k].first), set_moves[k].second);
      }
    }
    std::ranges::stable_sort(sets, [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
    std::vector<std::pair<double, Build>> set_looks(std::min(looks, sets.size()));
    ParallelFor(set_looks.size(),
                [&](std::size_t i) { set_looks[i] = deep(std::get<1>(sets[i]), {std::get<2>(sets[i])}); });
    for (const auto& next : set_looks) {
      take(next);
    }
    ++units;
    at(0, improved);
    if (!improved) {
      break;
    }
  }
  return {current_dps, current};
}

// Abilities to start the search from besides the start's: for each stat
// asked for, the one whose DPS grows the most with a lot more of it.
std::vector<std::string> BuildSearch::Seeds() {
  const std::vector<std::string>& abilities = p_.candidates[1];
  if (p_.thorough) {
    return abilities;
  }
  constexpr int kLots = 60;
  std::vector<std::string> out;
  for (std::size_t stat = 0; stat < kOptimizerStats.size(); ++stat) {
    if (!p_.restart_stats[stat]) {
      continue;
    }
    std::vector<Build> builds(abilities.size(), p_.start);
    for (std::size_t k = 0; k < abilities.size(); ++k) {
      builds[k].items[1] = abilities[k];
      Adapt(builds[k], 1);
    }
    Stats extra{};
    extra[stat] = kLots;
    const std::vector<std::optional<double>> plain = ScoreAll(builds);
    const std::vector<std::optional<double>> more = ScoreAll(builds, extra);
    std::vector<std::pair<double, std::string>> gains;
    for (std::size_t k = 0; k < abilities.size(); ++k) {
      if (plain[k] && more[k] && Better(*more[k], *plain[k])) {
        gains.emplace_back(*more[k] - *plain[k], abilities[k]);
      }
    }
    std::ranges::stable_sort(gains, [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < gains.size() && i < 1; ++i) {
      if (gains[i].second != p_.start.items[1] && std::ranges::find(out, gains[i].second) == out.end()) {
        out.push_back(gains[i].second);
      }
    }
  }
  return out;
}

OptimizerResult BuildSearch::Results() {
  OptimizerResult out;
  out.baseline = baseline_;
  {
    std::lock_guard lock(mu_);
    // Distinct builds, since two threads can score the same one at once.
    out.evaluated = static_cast<long>(scores_.size());
    for (const auto& [key, entry] : seen_) {
      out.builds.push_back(entry);
    }
  }
  std::ranges::stable_sort(out.builds, [](const auto& a, const auto& b) { return a.first > b.first; });
  if (out.builds.size() > p_.results) {
    out.builds.resize(p_.results);
  }
  return out;
}

void BuildSearch::Report() {
  const auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration<double>(now - last_report_).count() >= p_.progress_every_s) {
    last_report_ = now;
    double done = 0;
    double total = 0;
    {
      std::lock_guard lock(mu_);
      for (std::size_t i = 0; i < parts_done_.size(); ++i) {
        done += parts_done_[i] * part_sizes_[i];
        total += part_sizes_[i];
      }
    }
    try {
      p_.progress(total > 0 ? done / total : 0, Results());
    } catch (...) {
      stopped_by_ = std::current_exception();
      stopping_ = true;
      throw;
    }
  }
}

std::optional<OptimizerResult> BuildSearch::Run() {
  try {
    return Search();
  } catch (const Stopped&) {
    // A thread that stopped because another's report threw.
    std::rethrow_exception(stopped_by_);
  }
}

std::optional<OptimizerResult> BuildSearch::Search() {
  const auto baseline = Score(p_.start);
  if (!baseline) {
    return std::nullopt;
  }
  baseline_ = *baseline;
  // Some builds only work around one ability, like stacking the stat it
  // scales with: each change on the way there looks worse on its own. So
  // the search starts again from the abilities whose DPS grows most with
  // each stat, or from every ability in a thorough search.
  const std::vector<std::string> seeds = Seeds();
  // The descents from the start and from each seed are independent, so
  // they run at once. Shares of the search for progress reports, from
  // timing every class: one from a seed takes a few times less than the
  // one from the start, as most of what it looks at is already scored, and
  // trading enchantments very little.
  constexpr double kSeedDescent = 0.15;
  constexpr double kExchange = 0.03;
  {
    std::lock_guard lock(mu_);
    part_sizes_.assign(seeds.size() + 2, kSeedDescent);
    part_sizes_.front() = 1;
    part_sizes_.back() = kExchange;
    parts_done_.assign(part_sizes_.size(), 0);
  }
  ParallelFor(seeds.size() + 1, [&](std::size_t i) {
    Build from = p_.start;
    if (i > 0) {
      from.items[1] = seeds[i - 1];
      Adapt(from, 1);
    }
    if (Score(from)) {
      Descend(from, i);
    }
    std::lock_guard lock(mu_);
    parts_done_[i] = 1;
  });
  // The best few sets of items found, wherever the searches ended, get
  // their enchantments traded around.
  std::vector<std::pair<double, Build>> top;
  {
    std::lock_guard lock(mu_);
    for (const auto& [key, entry] : seen_) {
      top.push_back(entry);
    }
  }
  std::ranges::stable_sort(top, [](const auto& a, const auto& b) { return a.first > b.first; });
  constexpr std::size_t kTraded = 6;
  if (top.size() > kTraded) {
    top.resize(kTraded);
  }
  ParallelFor(top.size(), [&](std::size_t i) {
    Exchange(top[i].second);
    std::lock_guard lock(mu_);
    parts_done_.back() += 1.0 / kTraded;
  });
  return Results();
}

}  // namespace

std::optional<OptimizerResult> SearchBuilds(const Dataset& dataset, const OptimizerProblem& problem) {
  return BuildSearch(dataset, problem).Run();
}

}  // namespace rotmg
