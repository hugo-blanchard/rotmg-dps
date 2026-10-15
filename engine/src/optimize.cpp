#include "rotmg/optimize.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "parallel.hpp"
#include "rotmg/dataset.hpp"
#include "rotmg/enchant.hpp"
#include "rotmg/evaluate.hpp"

namespace rotmg {
namespace {

constexpr std::size_t kSlotCount = 4;
// ATT's index in kOptimizerStats.
constexpr std::size_t kAtt = 0;
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
    candidates_ = p_.candidates;
    // The most enchantments can add to each stat: each item holds at most
    // one single-stat and one dual-stat enchantment, and there are four.
    std::array<double, kOptimizerStats.size()> single{};
    std::array<double, kOptimizerStats.size()> dual{};
    for (const Enchantment& e : d_.enchantments) {
      if (Legacy(e)) {
        continue;
      }
      const bool is_dual = std::ranges::find(e.labels, "DUALSTAT") != e.labels.end();
      for (const Mutator& m : e.mutators) {
        const int stat = StatIndex(m.attributes.Text("stat"));
        if (m.tag == "ActivateOnEquip" && m.text == "IncrementStat" && stat >= 0) {
          double& most = is_dual ? dual[static_cast<std::size_t>(stat)] : single[static_cast<std::size_t>(stat)];
          most = std::max(most, m.attributes.Number("amount").value_or(0));
        }
      }
    }
    for (std::size_t s = 0; s < kOptimizerStats.size(); ++s) {
      max_extra_[s] = 4 * (single[s] + dual[s]);
    }
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
  bool Redundant(const Build& b) const;
  std::optional<double> Score(const Build& b, const Stats& extra = {});
  std::vector<std::optional<double>> ScoreAll(const std::vector<Build>& builds, const Stats& extra = {});
  Rates StatRates(const Build& b);
  Mask Sensitive(const Build& b);
  const Enchantment* Enabler(const Build& b, std::size_t slot);
  std::vector<const Enchantment*> Relevant(const Build& b, std::size_t slot);
  std::vector<const Enchantment*> Shortlist(Build& b, std::size_t slot);
  double Fill(Build& b, std::size_t slot);
  std::pair<double, Build> LinearFill(Build b);
  double Exchange(Build& b);
  double FillAll(Build& b);
  std::vector<const Enchantment*> OwnEnchantments(std::size_t slot, const std::string& item);
  std::pair<double, Build> Descend(Build current, std::size_t index);
  std::vector<std::string> Seeds(const Build& from);
  void DescendFrom(const Build& from, const std::vector<Build>& others = {});

  // An item in its slot with enchantments it brings along: none, one of its
  // own, or a special one that helps it most at some stats.
  struct Variant {
    std::string item;
    std::vector<std::string> enchants;
    bool operator==(const Variant&) const = default;
  };
  using Lists = std::array<std::vector<Variant>, kSlotCount>;
  enum class Kind { kDual, kSingle, kShare };
  std::optional<double> Probe(const Build& b, const Stats& extra);
  std::vector<std::optional<double>> ProbeAll(const Build& b, const std::vector<Stats>& extras);
  std::vector<Variant> Variants(std::size_t slot, const Build& reference);
  std::vector<Stats> CurvePoints(std::size_t slot) const;
  std::vector<std::size_t> ScalingStats();
  Lists CurveShortlists();
  const Enchantment* StatEnchantment(std::size_t slot, const std::string& item, Kind kind, std::size_t stat,
                                     std::size_t from);
  const Enchantment* ProcEnchantment(std::size_t slot, const std::string& item, std::size_t stat,
                                     const std::vector<int>& worn);
  std::vector<Build> Plans(const Build& items);
  std::pair<double, Build> BestPlan(const Build& items);
  std::vector<Build> Combinations(const Lists& lists);
  std::optional<OptimizerResult> Search();
  OptimizerResult Results();
  void Report();
  static std::string Key(const Build& b);

  const Dataset& d_;
  const OptimizerProblem& p_;
  // The items each slot's descents try: all the problem's, then the short
  // lists' once the curves have made them.
  std::array<std::vector<std::string>, kSlotCount> candidates_;
  // Every item a curve pass short-listed, by slot.
  std::array<std::vector<std::string>, kSlotCount> shortlisted_;
  std::array<double, kOptimizerStats.size()> max_extra_{};
  // The stats some candidate's score depends on, in kOptimizerStats order:
  // what the curves, plans and specials spread the enchantment budget over.
  std::vector<std::size_t> scaling_;
  std::atomic<long> probes_ = 0;
  std::map<std::tuple<std::size_t, std::string, int, std::size_t, std::size_t>, const Enchantment*>
      stat_enchantments_;
  std::map<std::tuple<std::size_t, std::string, std::size_t>, std::vector<const Enchantment*>> procs_;
  std::map<const Enchantment*, StatEffect> effects_;
  std::map<std::pair<std::size_t, std::string>, Fitting> fitting_;
  // How many items carry each label.
  std::map<std::string, int> carriers_;
  std::unordered_map<std::string, std::optional<double>> scores_;
  std::unordered_map<std::string, Rates> rates_;
  std::map<std::string, std::pair<double, Build>> seen_;
  // Whether an enchantment that does more than add stats changes an
  // item's DPS, by slot, the items worn and enchantment. Keyed by all the
  // items: the answer depends on the rest of them, like an Alien boost on
  // the Alien gear worn, and keying by the item alone kept whichever build
  // a thread reached first.
  std::map<std::tuple<std::size_t, std::string, const Enchantment*>, bool> tested_;
  // Relevant's answers, by slot, the items worn and which stats matter.
  std::map<std::pair<std::size_t, std::string>, std::vector<const Enchantment*>> relevant_;
  // Enabler's answers, by slot and the items worn.
  std::map<std::pair<std::size_t, std::string>, const Enchantment*> enablers_;
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
  // The most progress reported yet. A descent's share is a guess that drops
  // when a round finds something better and needs another, but a report
  // never goes back.
  double reported_ = 0;
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

// Whether the build holds an item that differs from another only in the
// enchantments it alone takes, without any of them: then it's the other
// item's build, which the search tries anyway. The start's items stay.
bool BuildSearch::Redundant(const Build& b) const {
  for (std::size_t slot = 0; slot < b.items.size(); ++slot) {
    const auto it = p_.variants[slot].find(b.items[slot]);
    if (it == p_.variants[slot].end() || b.items[slot] == p_.start.items[slot]) {
      continue;
    }
    if (std::ranges::none_of(b.enchants[slot], [&](const std::string& id) {
          return std::ranges::find(it->second, id) != it->second.end();
        })) {
      return true;
    }
  }
  return false;
}

std::optional<double> BuildSearch::Score(const Build& b, const Stats& extra) {
  if (stopping_) {
    throw Stopped{};
  }
  if (Redundant(b)) {
    return std::nullopt;
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
  // score the same, the one with fewer enchantments stays, so a slot where
  // nothing adds anything is left empty rather than holding one that does
  // nothing, like a second copy of a proc that doesn't stack. Then the
  // first by key, whichever thread got there first, so the result doesn't
  // depend on the order.
  std::string items_key;
  for (const std::string& item : b.items) {
    items_key += item + "\n";
  }
  const auto count = [](const Build& x) {
    std::size_t n = 0;
    for (const auto& list : x.enchants) n += list.size();
    return n;
  };
  auto it = seen_.find(items_key);
  if (it == seen_.end() || it->second.first < *dps ||
      (it->second.first == *dps &&
       std::pair{count(b), key} < std::pair{count(it->second.second), Key(it->second.second)})) {
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

// When the items alone do no damage, like a weapon that doesn't reach the
// target without more range, nothing else on the slot shows what it adds
// until something makes the build hit. The enchantment that does the most
// on its own then stays on the slot while the others are tried. It's chosen
// on the items alone, so every build with these items gets the same one.
const Enchantment* BuildSearch::Enabler(const Build& b, std::size_t slot) {
  Build items = b;
  for (auto& list : items.enchants) list.clear();
  const auto key = std::pair{slot, Key(items)};
  {
    std::lock_guard lock(mu_);
    if (auto it = enablers_.find(key); it != enablers_.end()) {
      return it->second;
    }
  }
  const Enchantment* best = nullptr;
  if (!b.items[slot].empty() && Score(items).value_or(0) <= 0) {
    std::vector<const Enchantment*> tried;
    for (const Enchantment* e : Fits(slot, b.items[slot]).options) {
      if (!effects_.at(e).by_stats) {
        tried.push_back(e);
      }
    }
    std::vector<Build> ones(tried.size(), items);
    for (std::size_t i = 0; i < tried.size(); ++i) {
      ones[i].enchants[slot] = {tried[i]->id};
    }
    const std::vector<std::optional<double>> scored = ScoreAll(ones);
    double most = 0;
    for (std::size_t i = 0; i < tried.size(); ++i) {
      if (scored[i] && *scored[i] > most) {
        most = *scored[i];
        best = tried[i];
      }
    }
  }
  std::lock_guard lock(mu_);
  return enablers_.try_emplace(key, best).first->second;
}

namespace {

// The slot's enchantments to try e with: e and the slot's enabler, when
// there is one and they can share the item, or e alone.
std::vector<std::string> WithEnabler(const Enchantment* enabler, const Enchantment* e) {
  if (enabler != nullptr && enabler != e && !Conflict(*enabler, *e)) {
    std::vector<std::string> ids = {enabler->id, e->id};
    std::ranges::sort(ids);
    return ids;
  }
  return {e->id};
}

}  // namespace

// The enchantments worth trying on the slot's item in this build. One that
// only adds stats counts when it raises a stat the build's DPS depends on,
// so +DEF counts once something scales with DEF. Of those that only add
// flat stats and clash with the same others, one that adds at least as
// much of every stat that matters makes the other pointless (Attack Bonus
// IV over III). Anything else counts when it changes the DPS on its own.
std::vector<const Enchantment*> BuildSearch::Relevant(const Build& b, std::size_t slot) {
  const Enchantment* enabler = Enabler(b, slot);
  Build bare = b;
  bare.enchants[slot].clear();
  if (enabler != nullptr) {
    bare.enchants[slot] = {enabler->id};
  }
  const Mask matters = Sensitive(bare);
  unsigned bits = 0;
  for (std::size_t i = 0; i < matters.size(); ++i) {
    bits |= matters[i] ? 1u << i : 0u;
  }
  // Whether an enchantment changes anything is tried on the items alone,
  // so the answer is the same whichever build asks first.
  Build items = b;
  for (auto& list : items.enchants) list.clear();
  const std::string context = Key(items);
  const auto cache_key = std::pair{slot, context + std::to_string(bits)};
  {
    std::lock_guard lock(mu_);
    if (auto it = relevant_.find(cache_key); it != relevant_.end()) {
      return it->second;
    }
  }
  Build reference = items;
  if (enabler != nullptr) {
    reference.enchants[slot] = {enabler->id};
  }
  const double alone = Score(items).value_or(0);
  const double with_enabler = Score(reference).value_or(0);
  // Whether e changes the DPS of what it's tried with.
  const auto changes_it = [&](const Enchantment* e, const std::optional<double>& s) {
    const std::vector<std::string> ids = WithEnabler(enabler, e);
    const double without = ids.size() > 1 ? with_enabler : alone;
    return s && std::abs(*s - without) > without * 1e-9;
  };
  // Enchantments that do more than add stats, each on its own, at once.
  {
    std::vector<const Enchantment*> untested;
    const Fitting& fits = Fits(slot, b.items[slot]);
    {
      std::lock_guard lock(mu_);
      for (const Enchantment* e : fits.options) {
        if (!effects_.at(e).by_stats && !tested_.contains(std::tuple{slot, context, e})) {
          untested.push_back(e);
        }
      }
    }
    std::vector<Build> ones(untested.size(), items);
    for (std::size_t i = 0; i < untested.size(); ++i) {
      ones[i].enchants[slot] = WithEnabler(enabler, untested[i]);
    }
    const std::vector<std::optional<double>> scored = ScoreAll(ones);
    std::lock_guard lock(mu_);
    for (std::size_t i = 0; i < untested.size(); ++i) {
      tested_.try_emplace(std::tuple{slot, context, untested[i]}, changes_it(untested[i], scored[i]));
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
    const auto key = std::tuple{slot, context, e};
    std::optional<bool> changes;
    {
      std::lock_guard lock(mu_);
      if (auto it = tested_.find(key); it != tested_.end()) {
        changes = it->second;
      }
    }
    if (!changes) {
      Build one = items;
      one.enchants[slot] = WithEnabler(enabler, e);
      changes = changes_it(e, Score(one));
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
  const Enchantment* enabler = Enabler(b, slot);
  Build base = b;
  if (enabler != nullptr) {
    base.enchants[slot] = {enabler->id};
  }
  const double bare = Score(base).value_or(0);
  std::vector<const Enchantment*> options = Relevant(b, slot);
  const std::size_t extra = p_.thorough ? 24 : 12;
  const Rates rates = StatRates(base);
  std::vector<Build> ones;
  for (const Enchantment* e : options) {
    if (!effects_.at(e).flat_only) {
      ones.push_back(b);
      ones.back().enchants[slot] = WithEnabler(enabler, e);
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
      for (const std::string& name : candidates_[slot]) {
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
        const auto& options = candidates_[slot < kSlotCount ? slot : 0];
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
std::vector<std::string> BuildSearch::Seeds(const Build& from) {
  const std::vector<std::string>& abilities = candidates_[1];
  if (p_.thorough) {
    return abilities;
  }
  constexpr int kLots = 60;
  std::vector<std::string> out;
  for (std::size_t stat = 0; stat < kOptimizerStats.size(); ++stat) {
    if (!p_.restart_stats[stat]) {
      continue;
    }
    std::vector<Build> builds(abilities.size(), from);
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
      if (gains[i].second != from.items[1] && std::ranges::find(out, gains[i].second) == out.end()) {
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
    out.evaluated = static_cast<long>(scores_.size()) + probes_;
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
    reported_ = std::max(reported_, total > 0 ? done / total : 0);
    try {
      p_.progress(reported_, Results());
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
  // Without enchantments to choose, the step-by-step search alone finds
  // the same builds as the curves, many times faster. Built around nothing,
  // the request asks only for its own build improved, which is that search
  // from the start.
  if (p_.enchantments != Mode::kBest || std::ranges::none_of(p_.restart_stats, [](bool b) { return b; })) {
    DescendFrom(p_.start);
    return Results();
  }
  // Shares of the search for progress reports, from timing every class:
  // the curves, the combinations and their fills, then the descents.
  constexpr double kCurves = 0.25;
  constexpr double kCombinations = 0.3;
  {
    std::lock_guard lock(mu_);
    part_sizes_ = {kCurves, kCombinations};
    parts_done_ = {0, 0};
  }
  // Which items are worth a look, by their damage curves, and the most
  // promising sets of them, each filled from its best enchantment plan.
  scaling_ = ScalingStats();
  const Lists lists = CurveShortlists();
  const std::vector<Build> starts = Combinations(lists);
  {
    std::lock_guard lock(mu_);
    parts_done_[0] = 1;
  }
  ParallelFor(starts.size(), [&](std::size_t i) {
    // The fill improves one item at a time from the plan; when it ends up
    // worse than the plan, the plan is what gets traded.
    Build filled = starts[i];
    const double fill = FillAll(filled);
    Build plan = starts[i];
    Exchange(fill > Score(plan).value_or(0) ? filled : plan);
    std::lock_guard lock(mu_);
    parts_done_[1] += 1.0 / static_cast<double>(starts.size());
  });
  // Then the step-by-step search over the short lists from the best build
  // so far: it carries enchantments from one item to the next, which finds
  // what the plans miss.
  for (std::size_t s = 0; s < kSlotCount; ++s) {
    if (candidates_[s].size() > 1 && !shortlisted_[s].empty()) {
      candidates_[s] = shortlisted_[s];
    }
  }
  Build best = p_.start;
  // The best build of other items close behind gets a descent too: a set
  // filled around the wrong ability can be one change from the top.
  std::vector<Build> others;
  {
    std::lock_guard lock(mu_);
    double top = -1;
    for (const auto& [key, entry] : seen_) {
      if (entry.first > top) {
        top = entry.first;
        best = entry.second;
      }
    }
    const auto differ = [&](const Build& b) {
      return std::ranges::count_if(std::views::iota(std::size_t{0}, kSlotCount),
                                   [&](std::size_t s) { return b.items[s] != best.items[s]; });
    };
    double runner = top * 0.97;
    for (const auto& [key, entry] : seen_) {
      if (entry.first > runner && differ(entry.second) >= 2) {
        runner = entry.first;
        others = {entry.second};
      }
    }
    parts_done_[1] = 1;
  }
  for (std::size_t s = 0; s < kSlotCount; ++s) {
    for (const Build* b : {&best, others.empty() ? &best : &others[0]}) {
      if (std::ranges::find(candidates_[s], b->items[s]) == candidates_[s].end()) {
        candidates_[s].push_back(b->items[s]);
      }
    }
  }
  DescendFrom(best, others);
  return Results();
}

// Descents from `from` and each of `others` over the search's candidates,
// then the best few sets of items found get their enchantments traded
// around. Some builds only work around one ability, like stacking the stat
// it scales with: each change on the way there looks worse on its own. So
// the descents also start again from the abilities whose DPS grows most
// with each stat asked for, or from every ability in a thorough search.
void BuildSearch::DescendFrom(const Build& from, const std::vector<Build>& others) {
  const std::vector<std::string> seeds = Seeds(from);
  // Shares of progress: the descent from `from` takes a few times more than
  // one from a seed, most of which is already scored, and trading
  // enchantments very little.
  constexpr double kDescent = 0.25;
  constexpr double kSeedDescent = 0.04;
  constexpr double kExchange = 0.05;
  std::size_t first = 0;
  {
    std::lock_guard lock(mu_);
    first = part_sizes_.size();
    part_sizes_.push_back(kDescent);
    for (std::size_t i = 0; i < seeds.size() + others.size(); ++i) {
      part_sizes_.push_back(kSeedDescent);
    }
    part_sizes_.push_back(kExchange);
    parts_done_.resize(part_sizes_.size(), 0);
  }
  // The descents are independent, so they run at once.
  ParallelFor(seeds.size() + others.size() + 1, [&](std::size_t i) {
    Build start = from;
    if (i > seeds.size()) {
      start = others[i - seeds.size() - 1];
    } else if (i > 0) {
      start.items[1] = seeds[i - 1];
      Adapt(start, 1);
    }
    if (Score(start)) {
      Descend(start, first + i);
    }
    std::lock_guard lock(mu_);
    parts_done_[first + i] = 1;
  });
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
}

// A build scored with extra stats for a curve, outside the score cache:
// each probe is asked once, and there are many.
std::optional<double> BuildSearch::Probe(const Build& b, const Stats& extra) {
  if (stopping_) {
    throw Stopped{};
  }
  if (Redundant(b)) {
    return std::nullopt;
  }
  if (p_.progress && !OnHelperThread()) {
    Report();
  }
  ++probes_;
  return p_.score(b, extra);
}

// The same at several stat points at once.
std::vector<std::optional<double>> BuildSearch::ProbeAll(const Build& b, const std::vector<Stats>& extras) {
  if (Redundant(b)) {
    return std::vector<std::optional<double>>(extras.size());
  }
  if (!p_.score_points) {
    std::vector<std::optional<double>> out;
    for (const Stats& extra : extras) out.push_back(Probe(b, extra));
    return out;
  }
  if (stopping_) {
    throw Stopped{};
  }
  if (p_.progress && !OnHelperThread()) {
    Report();
  }
  probes_ += static_cast<long>(extras.size());
  return p_.score_points(b, extras);
}

// The slot's candidates, each bare, with each of its own enchantments, and,
// for weapons and abilities, with the special enchantments that help it most
// on the reference build: a tradeoff like Overwhelming Strikes is worth far
// more on a weapon held back by its burst pause than by its fire rate.
std::vector<BuildSearch::Variant> BuildSearch::Variants(std::size_t slot, const Build& reference) {
  std::vector<Variant> out;
  const bool best = p_.enchantments == Mode::kBest;
  for (const std::string& name : candidates_[slot]) {
    out.push_back({name, {}});
    if (!best) {
      continue;
    }
    for (const Enchantment* e : OwnEnchantments(slot, name)) {
      out.push_back({name, {e->id}});
    }
  }
  if (!best || slot > 1) {
    return out;
  }
  // The best special can change with the stats, like one that slows the
  // weapon and only pays once DEX makes up for it, so it's picked in the
  // middle of the budget and again with each stat that matters at its most.
  Stats centre{};
  for (std::size_t stat : scaling_) {
    centre[stat] = static_cast<int>(max_extra_[stat] / 2);
  }
  std::vector<Stats> points = {centre};
  for (std::size_t stat : scaling_) {
    points.push_back(centre);
    points.back()[stat] = static_cast<int>(max_extra_[stat]);
  }
  std::vector<std::pair<std::size_t, const Enchantment*>> tries;
  const std::size_t bare_count = out.size();
  for (std::size_t i = 0; i < bare_count; ++i) {
    if (!out[i].enchants.empty()) {
      continue;
    }
    for (const Enchantment* e : Fits(slot, out[i].item).options) {
      const bool stats_only = !e->mutators.empty() && e->containers.empty() &&
                              std::ranges::all_of(e->mutators, [](const Mutator& m) {
                                return m.tag == "ActivateOnEquip" &&
                                       (m.text == "IncrementStat" || m.text == "BonusStatRelative");
                              });
      if (!stats_only) {
        tries.emplace_back(i, e);
      }
    }
  }
  const auto with = [&](std::size_t i, const Enchantment* e) {
    Build b = reference;
    b.items[slot] = out[i].item;
    b.enchants[slot] = e == nullptr ? std::vector<std::string>{} : std::vector<std::string>{e->id};
    return b;
  };
  const std::size_t n = points.size();
  std::vector<std::optional<double>> bare(bare_count * n);
  ParallelFor(bare_count, [&](std::size_t i) {
    const auto scores = ProbeAll(with(i, nullptr), points);
    std::ranges::copy(scores, bare.begin() + static_cast<std::ptrdiff_t>(i * n));
  });
  std::vector<std::optional<double>> gained(tries.size() * n);
  ParallelFor(tries.size(), [&](std::size_t t) {
    const auto scores = ProbeAll(with(tries[t].first, tries[t].second), points);
    std::ranges::copy(scores, gained.begin() + static_cast<std::ptrdiff_t>(t * n));
  });
  for (std::size_t p = 0; p < n; ++p) {
    std::map<std::size_t, std::pair<double, const Enchantment*>> best_special;
    for (std::size_t t = 0; t < tries.size(); ++t) {
      const auto [i, e] = tries[t];
      const auto& with_it = gained[t * n + p];
      const auto& without = bare[i * n + p];
      if (!with_it || !without || !Better(*with_it, *without)) {
        continue;
      }
      auto& [gain, which] = best_special[i];
      if (*with_it - *without > gain) {
        gain = *with_it - *without;
        which = e;
      }
    }
    for (const auto& [i, special] : best_special) {
      const bool known = std::ranges::any_of(out, [&](const Variant& v) {
        return v.item == out[i].item && v.enchants == std::vector<std::string>{special.second->id};
      });
      if (!known) {
        out.push_back({out[i].item, {special.second->id}});
      }
    }
  }
  return out;
}

// The stats any candidate's score rises with, each tried alone at the most
// enchantments can add to it, on the request's build without enchantments.
// Nothing about which stats matter is assumed: a new ability that scales
// with VIT makes VIT one of them.
std::vector<std::size_t> BuildSearch::ScalingStats() {
  // The search spreads the budget over this many stats at most, the ones
  // that raise a score the most: every one more multiplies the plans.
  constexpr std::size_t kMostScaling = 4;
  Build reference = p_.start;
  for (auto& list : reference.enchants) list.clear();
  std::vector<Stats> points = {Stats{}};
  std::vector<std::size_t> point_stat = {kOptimizerStats.size()};
  for (std::size_t s = 0; s < kOptimizerStats.size(); ++s) {
    if (max_extra_[s] > 0) {
      points.push_back(Stats{});
      points.back()[s] = static_cast<int>(max_extra_[s]);
      point_stat.push_back(s);
    }
  }
  std::vector<Build> builds;
  for (std::size_t slot = 0; slot < kSlotCount; ++slot) {
    for (const std::string& name : candidates_[slot]) {
      builds.push_back(reference);
      builds.back().items[slot] = name;
    }
  }
  std::vector<std::vector<std::optional<double>>> scores(builds.size());
  ParallelFor(builds.size(), [&](std::size_t i) { scores[i] = ProbeAll(builds[i], points); });
  // By the most any candidate gains from it, as a share of its score.
  std::array<double, kOptimizerStats.size()> gain{};
  for (const auto& row : scores) {
    for (std::size_t k = 1; k < row.size(); ++k) {
      if (row[0] && row[k] && *row[0] > 0 && Better(*row[k], *row[0])) {
        gain[point_stat[k]] = std::max(gain[point_stat[k]], *row[k] / *row[0] - 1);
      }
    }
  }
  std::vector<std::size_t> out;
  for (std::size_t s = 0; s < gain.size(); ++s) {
    if (gain[s] > 0) out.push_back(s);
  }
  std::ranges::stable_sort(out, std::greater<>{}, [&](std::size_t s) { return gain[s]; });
  if (out.size() > kMostScaling) {
    out.resize(kMostScaling);
  }
  std::ranges::sort(out);
  if (out.empty()) {
    out.push_back(kAtt);
  }
  return out;
}

// The stat points a slot's items are compared at, over the stats that
// matter. Enchantment slots are shared, so stats trade off: each point
// spends the same budget, all on one stat, half on each of two, or evenly
// on all. Abilities also get a line along each, one point at a time for
// their breakpoints, the rest of the budget spread over the others.
std::vector<Stats> BuildSearch::CurvePoints(std::size_t slot) const {
  const auto share = [&](std::size_t stat, double part) {
    return static_cast<int>(std::lround(max_extra_[stat] * part));
  };
  std::vector<Stats> out = {Stats{}};
  for (std::size_t s : scaling_) {
    for (double part : {0.5, 1.0}) {
      Stats e{};
      e[s] = share(s, part);
      out.push_back(e);
    }
  }
  for (std::size_t i = 0; i < scaling_.size(); ++i) {
    for (std::size_t j = i + 1; j < scaling_.size(); ++j) {
      Stats e{};
      e[scaling_[i]] = share(scaling_[i], 0.5);
      e[scaling_[j]] = share(scaling_[j], 0.5);
      out.push_back(e);
    }
  }
  if (scaling_.size() > 2) {
    Stats e{};
    for (std::size_t s : scaling_) e[s] = share(s, 1.0 / static_cast<double>(scaling_.size()));
    out.push_back(e);
  }
  if (slot == 1) {
    for (std::size_t s : scaling_) {
      const int top = std::max(1, share(s, 1.0));
      const int step = std::max(1, top / 12);
      const double others = static_cast<double>(scaling_.size() - 1);
      for (int v = 0; v <= top; v += step) {
        const double rest = 1 - static_cast<double>(v) / top;
        Stats e{};
        for (std::size_t o : scaling_) {
          if (o != s) e[o] = share(o, rest / others);
        }
        e[s] += v;
        out.push_back(e);
      }
    }
  }
  return out;
}

// Each slot's short list: the best variants of the best few items at some
// point of their curve, tried with the leading items of the other slots.
// Each list puts first the variants that lead somewhere, best at some point,
// so the other slots are tried with each kind of build (an ATT bow and a DEX
// bow, not two DEX bows); a spell that halves projectile speed is good with
// one and bad with the other.
BuildSearch::Lists BuildSearch::CurveShortlists() {
  Build reference = p_.start;
  for (auto& list : reference.enchants) list.clear();
  std::array<std::vector<Variant>, kSlotCount> variants;
  for (std::size_t s = 0; s < kSlotCount; ++s) {
    variants[s] = Variants(s, reference);
  }
  Lists lists;
  for (std::size_t s = 0; s < kSlotCount; ++s) {
    lists[s] = {{reference.items[s], {}}};
  }
  const std::size_t tops = p_.thorough ? 4 : 3;
  constexpr std::size_t kPartners = 2;
  // A slot whose partners are the same as last pass would come out the same.
  std::array<std::vector<std::array<Variant, kSlotCount>>, kSlotCount> last_contexts;
  for (int pass = 0; pass < 2; ++pass) {
    for (std::size_t slot = 0; slot < kSlotCount; ++slot) {
      if (candidates_[slot].size() <= 1) {
        continue;
      }
      // Weapons are tried with the leading abilities, abilities with the
      // leading weapons, armor and rings with pairs of both.
      const auto partners = [&](std::size_t s) {
        if (s == slot) {
          return std::vector<Variant>{Variant{}};
        }
        const std::size_t n = s <= 1 ? kPartners : 1;
        return std::vector<Variant>(lists[s].begin(), lists[s].begin() + static_cast<std::ptrdiff_t>(
                                                                              std::min(n, lists[s].size())));
      };
      std::vector<std::array<Variant, kSlotCount>> contexts;
      for (const Variant& w : partners(0))
        for (const Variant& a : partners(1))
          for (const Variant& ar : partners(2))
            for (const Variant& r : partners(3)) contexts.push_back({w, a, ar, r});
      if (contexts == last_contexts[slot]) {
        std::lock_guard lock(mu_);
        parts_done_[0] = std::min(1.0, parts_done_[0] + 1.0 / 8);
        continue;
      }
      last_contexts[slot] = contexts;
      const std::vector<Stats> points = CurvePoints(slot);
      const auto& vs = variants[slot];
      const std::size_t cells = contexts.size() * points.size();
      std::vector<std::vector<double>> score(vs.size(), std::vector<double>(cells, -1));
      ParallelFor(vs.size() * contexts.size(), [&](std::size_t n) {
        const std::size_t i = n / contexts.size();
        const std::size_t context = n % contexts.size();
        Build b;
        for (std::size_t s = 0; s < kSlotCount; ++s) {
          const Variant& v = s == slot ? vs[i] : contexts[context][s];
          b.items[s] = v.item;
          b.enchants[s] = v.enchants;
        }
        const auto scores = ProbeAll(b, points);
        for (std::size_t p = 0; p < points.size(); ++p) {
          if (scores[p]) {
            score[i][context * points.size() + p] = *scores[p];
          }
        }
      });
      std::set<std::size_t> kept;
      std::set<std::size_t> leaders;
      std::vector<std::size_t> order(vs.size());
      for (std::size_t cell = 0; cell < cells; ++cell) {
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return score[a][cell] > score[b][cell]; });
        // The best few items, each with its best variant here, and any
        // variant within 2% of the best.
        std::set<std::string> items_here;
        for (std::size_t r = 0; r < order.size() && score[order[r]][cell] > 0; ++r) {
          const bool close = score[order[r]][cell] >= score[order[0]][cell] * 0.98;
          const bool seen = items_here.contains(vs[order[r]].item);
          if (!close && items_here.size() >= tops) {
            break;
          }
          if (close || !seen) {
            kept.insert(order[r]);
            items_here.insert(vs[order[r]].item);
          }
        }
        if (!order.empty() && score[order[0]][cell] > 0) {
          leaders.insert(order[0]);
        }
      }
      std::vector<std::tuple<bool, double, std::size_t>> ranked;
      for (std::size_t i : kept) {
        ranked.emplace_back(leaders.contains(i), *std::ranges::max_element(score[i]), i);
      }
      std::ranges::sort(ranked, std::greater<>());
      lists[slot].clear();
      for (const auto& [leads, best, i] : ranked) {
        lists[slot].push_back(vs[i]);
        if (std::ranges::find(shortlisted_[slot], vs[i].item) == shortlisted_[slot].end()) {
          shortlisted_[slot].push_back(vs[i].item);
        }
      }
      std::lock_guard lock(mu_);
      parts_done_[0] = std::min(1.0, parts_done_[0] + 1.0 / 8);
    }
  }
  return lists;
}

// The enchantment that adds the most of a stat to the item: a dual-stat
// one, a flat single-stat one, or a share of `from` turned into `stat`.
const Enchantment* BuildSearch::StatEnchantment(std::size_t slot, const std::string& item, Kind kind,
                                                std::size_t stat, std::size_t from) {
  const auto key = std::tuple{slot, item, static_cast<int>(kind), stat, from};
  {
    std::lock_guard lock(mu_);
    if (auto it = stat_enchantments_.find(key); it != stat_enchantments_.end()) {
      return it->second;
    }
  }
  const Enchantment* best = nullptr;
  double most = 0;
  for (const Enchantment* e : Fits(slot, item).options) {
    const bool dual = std::ranges::find(e->labels, "DUALSTAT") != e->labels.end();
    const bool single = std::ranges::find(e->labels, "SINGLESTAT") != e->labels.end();
    if ((kind == Kind::kDual) != dual || (kind != Kind::kDual && !single) || e->mutators.empty() ||
        !e->containers.empty()) {
      continue;
    }
    double amount = 0;
    bool only_this = true;
    for (const Mutator& m : e->mutators) {
      const bool equip = m.tag == "ActivateOnEquip";
      if (equip && kind == Kind::kShare && m.text == "BonusStatRelative" &&
          m.attributes.Text("stat") == kOptimizerStats[stat] &&
          m.attributes.Text("statRelativeTo") == kOptimizerStats[from]) {
        amount += m.attributes.Number("amount").value_or(0);
      } else if (equip && kind != Kind::kShare && m.text == "IncrementStat") {
        if (m.attributes.Text("stat") == kOptimizerStats[stat]) {
          amount += m.attributes.Number("amount").value_or(0);
        }
      } else {
        only_this = false;
      }
    }
    if (only_this && amount > most) {
      best = e;
      most = amount;
    }
  }
  std::lock_guard lock(mu_);
  return stat_enchantments_.try_emplace(key, best).first->second;
}

// The on-shoot boost of a stat that gives the item the most of it, whose
// gear condition (like wearing an Alien item) `worn` meets.
const Enchantment* BuildSearch::ProcEnchantment(std::size_t slot, const std::string& item, std::size_t stat,
                                                const std::vector<int>& worn) {
  const auto key = std::tuple{slot, item, stat};
  std::vector<const Enchantment*> procs;
  bool known = false;
  {
    std::lock_guard lock(mu_);
    if (auto it = procs_.find(key); it != procs_.end()) {
      procs = it->second;
      known = true;
    }
  }
  if (!known) {
    // By how much of the stat each keeps up while it lasts.
    std::vector<std::pair<double, const Enchantment*>> ranked;
    for (const Enchantment* e : Fits(slot, item).options) {
      if (e->mutators.size() != 1 || !e->containers.empty()) continue;
      const Mutator& m = e->mutators[0];
      if (m.tag == "AddOnPlayerShootActivate" && m.text == "StatBoostSelf" &&
          m.attributes.Text("stat") == kOptimizerStats[stat]) {
        ranked.emplace_back(m.attributes.Number("amount").value_or(0) * m.attributes.Number("duration").value_or(0),
                            e);
      }
    }
    std::ranges::stable_sort(ranked, std::greater<>{}, &std::pair<double, const Enchantment*>::first);
    for (const auto& [value, e] : ranked) procs.push_back(e);
    std::lock_guard lock(mu_);
    procs_.try_emplace(key, procs);
  }
  for (const Enchantment* e : procs) {
    if (GearWorn(e->mutators[0].attributes, worn)) return e;
  }
  return nullptr;
}

// Consistent enchantment plans for one set of items: every item stacks the
// same stat with its dual-stat enchantment and takes the same kind of
// single-stat one (more of that stat, a share of it into itself, or a share
// of it turned into another stat), with and without its special enchantment,
// and any room left takes on-shoot boosts, each stat once when its boosts
// don't stack. A conversion only pays once the stat it reads is stacked on several
// items, which swapping one enchantment at a time never reaches.
std::vector<Build> BuildSearch::Plans(const Build& items) {
  // Each plan stacks a stat that matters, and its single-stat enchantment
  // is more of that stat, a share of it into itself, or a share of it
  // turned into another stat that matters.
  constexpr std::size_t kItself = kOptimizerStats.size();
  std::vector<std::pair<Kind, std::size_t>> singles = {{Kind::kSingle, kItself}, {Kind::kShare, kItself}};
  for (std::size_t into : scaling_) singles.emplace_back(Kind::kShare, into);
  // The items' own enchantments (their special or awakening).
  std::array<std::vector<const Enchantment*>, kSlotCount> kept;
  for (std::size_t s = 0; s < kSlotCount; ++s) {
    for (const std::string& id : items.enchants[s]) {
      const auto it = std::ranges::find(d_.enchantments, id, &Enchantment::id);
      if (it != d_.enchantments.end()) kept[s].push_back(&*it);
    }
  }
  std::vector<int> worn;
  for (std::size_t s = 0; s < kSlotCount; ++s) {
    const std::string& name = items.items[s];
    if (name.empty()) continue;
    if (s == 0) {
      if (const Weapon* w = FindWeapon(d_, name)) worn.push_back(w->type);
    } else if (s == 1) {
      if (const Ability* a = FindAbility(d_, name)) worn.push_back(a->type);
    } else if (const Gear* g = FindGear(s == 2 ? d_.armors : d_.rings, name)) {
      worn.push_back(g->type);
    }
  }
  std::vector<Build> out;
  // Each plan with the items' own enchantments and, when there are any,
  // without them, for the room they take: a bow can be worth more holding
  // two WIS enchantments for its ability than its own special.
  const bool any_kept = std::ranges::any_of(kept, [](const auto& list) { return !list.empty(); });
  for (bool keep : {true, false}) {
    if (!keep && !any_kept) {
      continue;
    }
    for (std::size_t stat : scaling_) {
      for (const auto& [kind, into] : singles) {
        if (kind == Kind::kShare && into == stat) continue;
        Build b = items;
        std::vector<std::size_t> boosts;
        boosts.push_back(stat);
        for (std::size_t boost : scaling_) {
          if (boost != stat) boosts.push_back(boost);
        }
        for (std::size_t s = 0; s < kSlotCount; ++s) {
          if (b.items[s].empty()) {
            continue;
          }
          const int room = std::min(p_.per_item, Fits(s, b.items[s]).slots);
          std::vector<const Enchantment*> chosen;
          const auto add = [&](const Enchantment* e) {
            if (e == nullptr || std::cmp_greater_equal(chosen.size(), room) ||
                std::ranges::any_of(chosen, [&](const Enchantment* c) { return c == e || Conflict(*c, *e); })) {
              return;
            }
            chosen.push_back(e);
          };
          if (keep) {
            for (const Enchantment* e : kept[s]) add(e);
          }
          add(StatEnchantment(s, b.items[s], Kind::kDual, stat, stat));
          add(StatEnchantment(s, b.items[s], kind, into == kItself ? stat : into, stat));
          for (auto it = boosts.begin(); it != boosts.end(); ++it) {
            const std::size_t before = chosen.size();
            add(ProcEnchantment(s, b.items[s], *it, worn));
            if (chosen.size() > before) {
              // Boosts of the same stat don't add up when the data says so.
              if (chosen.back()->mutators[0].attributes.Text("channel") == "noStack") {
                boosts.erase(it);
              }
              break;
            }
          }
          b.enchants[s].clear();
          for (const Enchantment* e : chosen) b.enchants[s].push_back(e->id);
        }
        out.push_back(std::move(b));
      }
    }
  }
  return out;
}

// A set of items' best plan and its score; without enchantments to choose,
// the items as they are.
std::pair<double, Build> BuildSearch::BestPlan(const Build& items) {
  if (p_.enchantments != Mode::kBest) {
    return {Score(items).value_or(-1), items};
  }
  std::pair<double, Build> best = {-1, items};
  const std::vector<Build> plans = Plans(items);
  const std::vector<std::optional<double>> scores = ScoreAll(plans);
  for (std::size_t i = 0; i < plans.size(); ++i) {
    if (scores[i] && *scores[i] > best.first) {
      best = {*scores[i], plans[i]};
    }
  }
  return best;
}

// The most promising sets of items, with their best plans. Weapon and
// ability pairs come first, with the leading armor and ring; then each of
// the best pairs gets its own best armors and rings; and every set's pieces
// are tried together, the slots they leave with the best combinations' items.
std::vector<Build> BuildSearch::Combinations(const Lists& lists) {
  using Widths = std::array<std::size_t, kSlotCount>;
  const Widths width = p_.thorough ? Widths{14, 16, 16, 24} : Widths{10, 12, 12, 16};
  // Distinct items per slot, each with its best variant.
  Lists pool;
  for (std::size_t s = 0; s < kSlotCount; ++s) {
    std::set<std::string> seen;
    for (const Variant& v : lists[s]) {
      if (seen.insert(v.item).second) pool[s].push_back(v);
    }
    if (pool[s].empty()) {
      pool[s] = {{p_.start.items[s], {}}};
    }
  }
  std::array<std::size_t, kSlotCount> lim;
  for (std::size_t s = 0; s < kSlotCount; ++s) lim[s] = std::min(width[s], pool[s].size());
  // Each set's pieces, the two best of a slot that takes several.
  const auto rank_of = [&](std::size_t slot, const std::string& name) {
    const auto it = std::ranges::find(pool[slot], name, &Variant::item);
    return static_cast<std::size_t>(it - pool[slot].begin());
  };
  std::vector<std::array<std::vector<std::size_t>, kSlotCount>> set_rows;
  for (const EquipmentSet& set : d_.sets) {
    std::array<std::vector<std::string>, kSlotCount> pieces;
    for (const EquipmentSet::Piece& piece : set.pieces) {
      const auto slot = static_cast<std::size_t>(piece.slot);
      if (slot >= kSlotCount || candidates_[slot].size() <= 1 ||
          std::ranges::find(candidates_[slot], piece.name) == candidates_[slot].end()) {
        continue;
      }
      // Only the key piece makes a set with key pieces count.
      if (slot == 3 && !set.key_pieces.empty() &&
          std::ranges::find(set.key_pieces, piece.item_type) == set.key_pieces.end()) {
        continue;
      }
      if (std::ranges::find(pieces[slot], piece.name) == pieces[slot].end()) pieces[slot].push_back(piece.name);
    }
    std::array<std::vector<std::size_t>, kSlotCount> row;
    int covered = 0;
    for (std::size_t slot = 0; slot < kSlotCount; ++slot) {
      auto& names = pieces[slot];
      if (names.empty()) continue;
      std::ranges::stable_sort(names,
                               [&](const auto& a, const auto& b) { return rank_of(slot, a) < rank_of(slot, b); });
      if (names.size() > 2) names.resize(2);
      for (const std::string& name : names) {
        if (rank_of(slot, name) == pool[slot].size()) pool[slot].push_back({name, {}});
        row[slot].push_back(rank_of(slot, name));
      }
      ++covered;
    }
    if (covered >= 2) set_rows.push_back(row);
  }
  using Combo = std::array<std::size_t, kSlotCount>;
  const auto build_of = [&](const Combo& c) {
    Build b = p_.start;
    for (std::size_t s = 0; s < kSlotCount; ++s) {
      b.items[s] = pool[s][c[s]].item;
      if (p_.enchantments == Mode::kBest) {
        b.enchants[s] = pool[s][c[s]].enchants;
      } else {
        Adapt(b, s);
      }
    }
    return b;
  };
  std::map<Combo, std::pair<double, Build>> rated;
  // Each weapon's and ability's enchantments of its own to plan around:
  // the curves' variants, or for a slot they didn't look at (a locked one),
  // its variants now. A weapon's best special can depend on the rest of
  // the build, like a slow, hard-hitting one that needs DEX from the plan.
  std::array<std::map<std::string, std::vector<std::vector<std::string>>>, 2> specials;
  if (p_.enchantments == Mode::kBest) {
    Build reference = p_.start;
    for (auto& list : reference.enchants) list.clear();
    for (std::size_t s = 0; s < 2; ++s) {
      const std::vector<Variant> all = candidates_[s].size() <= 1 ? Variants(s, reference) : lists[s];
      for (const Variant& v : all) {
        auto& known = specials[s][v.item];
        if (std::ranges::find(known, v.enchants) == known.end()) known.push_back(v.enchants);
      }
    }
  }
  // A combination's best plan, with each of its weapon's and ability's
  // specials in turn.
  const auto best_plan = [&](const Combo& c) {
    const Build b = build_of(c);
    std::pair<double, Build> best = BestPlan(b);
    for (std::size_t s = 0; s < 2; ++s) {
      const auto it = specials[s].find(b.items[s]);
      if (it == specials[s].end()) continue;
      for (const auto& enchants : it->second) {
        if (enchants == b.enchants[s]) continue;
        Build other = b;
        other.enchants[s] = enchants;
        if (auto plan = BestPlan(other); plan.first > best.first) best = std::move(plan);
      }
    }
    return best;
  };
  const auto rate = [&](std::vector<Combo> list) {
    std::ranges::sort(list);
    list.erase(std::unique(list.begin(), list.end()), list.end());
    std::erase_if(list, [&](const Combo& c) { return rated.contains(c); });
    std::vector<std::pair<double, Build>> found(list.size());
    ParallelFor(list.size(), [&](std::size_t i) { found[i] = best_plan(list[i]); });
    for (std::size_t i = 0; i < list.size(); ++i) rated.emplace(list[i], std::move(found[i]));
  };
  const auto best_first = [&](std::vector<Combo> list) {
    rate(list);
    std::ranges::stable_sort(list,
                             [&](const Combo& a, const Combo& b) { return rated.at(a).first > rated.at(b).first; });
    return list;
  };
  std::vector<Combo> pairs;
  for (std::size_t a = 0; a < lim[0]; ++a)
    for (std::size_t b = 0; b < lim[1]; ++b) pairs.push_back({a, b, 0, 0});
  // The best pairs, but no ability or weapon more than a few times: when
  // the weapon hardly matters (an ability-weighted search) the top pairs
  // would otherwise be one ability with interchangeable weapons.
  const std::size_t pair_count = p_.thorough ? 20 : 10;
  std::map<std::size_t, std::size_t> per_weapon;
  std::map<std::size_t, std::size_t> per_ability;
  std::vector<Combo> top_pairs;
  for (const Combo& c : best_first(pairs)) {
    if (top_pairs.size() >= pair_count) break;
    if (per_ability[c[1]] >= 2 || per_weapon[c[0]] >= 3) continue;
    ++per_ability[c[1]];
    ++per_weapon[c[0]];
    top_pairs.push_back(c);
  }
  constexpr std::size_t kKeep = 3;
  std::vector<Combo> combos;
  for (const Combo& pair : top_pairs) {
    std::vector<Combo> armors;
    std::vector<Combo> rings;
    for (std::size_t c = 0; c < lim[2]; ++c) armors.push_back({pair[0], pair[1], c, 0});
    for (std::size_t r = 0; r < lim[3]; ++r) rings.push_back({pair[0], pair[1], 0, r});
    armors = best_first(armors);
    rings = best_first(rings);
    for (std::size_t i = 0; i < kKeep && i < armors.size(); ++i) {
      for (std::size_t j = 0; j < kKeep && j < rings.size(); ++j) {
        combos.push_back({pair[0], pair[1], armors[i][2], rings[j][3]});
      }
    }
  }
  std::vector<std::vector<Combo>> set_combos;
  for (const auto& row : set_rows) {
    set_combos.emplace_back();
    // A slot the set leaves free takes the items of the best combinations
    // so far and the slot's leading items.
    std::array<std::vector<std::size_t>, kSlotCount> choices;
    for (std::size_t s = 0; s < kSlotCount; ++s) {
      choices[s] = row[s];
      for (std::size_t k = 0; row[s].empty() && k < combos.size() && k < 3; ++k) {
        if (std::ranges::find(choices[s], combos[k][s]) == choices[s].end()) choices[s].push_back(combos[k][s]);
      }
      for (std::size_t k = 0; row[s].empty() && k < lim[s] && k < 3; ++k) {
        if (std::ranges::find(choices[s], k) == choices[s].end()) choices[s].push_back(k);
      }
      if (choices[s].empty()) choices[s] = {0};
    }
    for (std::size_t a : choices[0])
      for (std::size_t b : choices[1])
        for (std::size_t c : choices[2])
          for (std::size_t e : choices[3]) set_combos.back().push_back({a, b, c, e});
    combos.insert(combos.end(), set_combos.back().begin(), set_combos.back().end());
  }
  std::ranges::sort(combos);
  combos.erase(std::unique(combos.begin(), combos.end()), combos.end());
  combos = best_first(combos);
  const std::size_t fills = p_.thorough ? 32 : 12;
  std::vector<Build> out;
  std::set<Combo> filled;
  for (std::size_t i = 0; i < combos.size() && out.size() < fills; ++i) {
    if (rated.at(combos[i]).first > 0) {
      out.push_back(rated.at(combos[i]).second);
      filled.insert(combos[i]);
    }
  }
  // A set's pieces often pay off only with enchantments the plans don't
  // try, like Alien gear turning a DEF boost into DEX, so the best few
  // sets get filled and the ones that fill best are kept.
  std::vector<Combo> set_bests;
  for (const std::vector<Combo>& list : set_combos) {
    const auto best = std::ranges::max_element(list, {}, [&](const Combo& c) { return rated.at(c).first; });
    if (best != list.end() && rated.at(*best).first > 0 && !filled.contains(*best)) set_bests.push_back(*best);
  }
  set_bests = best_first(set_bests);
  set_bests.erase(std::unique(set_bests.begin(), set_bests.end()), set_bests.end());
  constexpr std::size_t kSetTries = 6;
  constexpr std::size_t kSetFills = 3;
  if (set_bests.size() > kSetTries) set_bests.resize(kSetTries);
  std::vector<std::pair<double, std::size_t>> set_fills(set_bests.size());
  ParallelFor(set_bests.size(), [&](std::size_t i) {
    Build b = rated.at(set_bests[i]).second;
    set_fills[i] = {FillAll(b), i};
  });
  std::ranges::stable_sort(set_fills, std::greater<>{}, &std::pair<double, std::size_t>::first);
  for (std::size_t i = 0; i < set_fills.size() && i < kSetFills; ++i) {
    out.push_back(rated.at(set_bests[set_fills[i].second]).second);
  }
  return out;
}

}  // namespace

std::optional<OptimizerResult> SearchBuilds(const Dataset& dataset, const OptimizerProblem& problem) {
  return BuildSearch(dataset, problem).Run();
}

}  // namespace rotmg
