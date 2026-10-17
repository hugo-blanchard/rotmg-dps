#include "enchantments.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <string_view>
#include <tuple>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../parallel.hpp"
#include "frontier.hpp"
#include "rotmg/enchant.hpp"
#include "rotmg/evaluate.hpp"
#include "rotmg/hits.hpp"

// How the search works. An item's enchantments fall in three kinds: ones
// that only add stats, specials (anything else the engine models, like a
// damage multiplier or an on-shoot boost) and ones that can't change the
// score. A layout is a choice of specials on every item; it fixes how the
// weapon and ability behave, so its score is a function of the character's
// stats alone, and only of the stats the score reads. More of any of those
// never lowers it, so a build with at least as much of every read stat as
// another is at least as good.
//
// For a layout, the stat enchantments on each item are chosen together:
// first each item's conversions ("16% of bonus ATT to DEX"), then its flat
// stats. Under one choice of conversions (an assignment) final stats only
// grow with each stat's flat sum, so flat sums are compared on the read
// stats and the stats the assignment converts from, and only sums that no
// other sum matches or beats are kept, item by item. The last item's sums
// are compared on their final stats. A sum whose best completion is matched
// by a final already reached is dropped early. What's left is every final
// that could be the best, and the layout's best build is among them.
//
// An item's own conversion can make a stat raise one read stat and lower
// another (Faded Heart Scarab: more DEX, less ATT). Sums then compare only
// when equal in it, so a sum is also dropped when another has enough of
// the rest to make up for it (Outdoes), and a best completion takes each
// read stat at its own best (Ceilings).
//
// Layouts that leave the same stat options open share these finals. Each
// layout's are scored with ceilings: the score at the most of every read
// stat of a group of finals bounds every final in the group.

namespace rotmg::search {

// Effect parameters by key, like a.Number(kCooldown).
using enum ParamKey;
namespace {

using Tenths = std::array<std::int32_t, kStats>;
using Mask = std::array<bool, kStats>;
// Tenths to points, rounded down as FinalStats does.
std::int32_t PointsOf(std::int32_t tenths) { return tenths >= 0 ? tenths / 10 : -((-tenths + 9) / 10); }

// The most each read stat reaches over every flat sum between `least` and
// `most`, under one choice of conversions. With no stat moving read stats
// both ways, that's the stats at one corner. Otherwise each read stat is
// bounded part by part: FinalStats floors its flat sum, the enchantments'
// conversions into it and the items' own apart, and each part grows with
// every stat it takes a positive share of and shrinks with every stat it
// takes a negative one of. A stat that does both within one part leaves
// that read stat with no best case.
class Ceilings {
 public:
  Ceilings(const std::vector<std::size_t>& read, const Mask& is_read, std::span<const Conversion> conversions,
           const StatBase& base)
      : read_(&read), base_(&base) {
    std::array<std::array<Mask, kStats>, 2> raises{};
    std::array<std::array<Mask, kStats>, 2> lowers{};
    Mask up = is_read;
    Mask down{};
    const auto note = [&](std::span<const Conversion> list, std::size_t part) {
      for (const Conversion& c : list) {
        if (!is_read[c.to] || c.percent == 0) continue;
        (c.percent > 0 ? raises : lowers)[part][c.to][c.from] = true;
        (c.percent > 0 ? up : down)[c.from] = true;
        if (!c.of_total && !base.has_class) continue;
        terms_[c.to][part].push_back({c.from, c.percent / 100, c.of_total, c.percent < 0});
      }
    };
    note(conversions, 0);
    note(base.own, 1);
    for (std::size_t s = 0; s < kStats; ++s) {
      mixed_ = mixed_ || (up[s] && down[s]);
      lower_[s] = down[s] && !up[s];
      for (std::size_t part = 0; part < 2; ++part) {
        for (const std::size_t r : read) bounded_ = bounded_ && !(raises[part][r][s] && lowers[part][r][s]);
      }
    }
  }

  bool bounded() const { return bounded_; }

  Stats Of(const Tenths& most, const Tenths& least, std::span<const Conversion> conversions) const {
    if (!mixed_) {
      Tenths corner;
      for (std::size_t s = 0; s < kStats; ++s) corner[s] = lower_[s] ? least[s] : most[s];
      return FinalStats(*base_, corner, conversions);
    }
    // As detail::Converted adds each share up, from the corner each one
    // needs.
    Stats out{};
    for (const std::size_t r : *read_) {
      out[r] = base_->before[r] + PointsOf(most[r]);
      for (const std::vector<Term>& part : terms_[r]) {
        double sum = 0;
        for (const Term& t : part) {
          const int everything = base_->before[t.from] + PointsOf(t.lowers ? least[t.from] : most[t.from]) +
                                 base_->item_bonuses[t.from];
          sum += t.share * (t.of_total ? everything : std::max(0, everything - base_->class_max[t.from]));
        }
        out[r] += static_cast<int>(std::floor(sum + 1e-9));
      }
    }
    return out;
  }

 private:
  struct Term {
    std::size_t from;
    double share;
    bool of_total;
    bool lowers;
  };

  const std::vector<std::size_t>* read_;
  const StatBase* base_;
  bool mixed_ = false;
  bool bounded_ = true;
  // The stats more of which only lower read stats.
  Mask lower_{};
  // Per read stat, the enchantments' conversions into it, then the items'
  // own.
  std::array<std::array<std::vector<Term>, 2>, kStats> terms_;
};

constexpr std::size_t kMaxCompared = 12;

// A stat option of one item: its enchantment, flat stats in tenths and the
// conversions into stats the score reads.
struct StatOption {
  std::int32_t enchantment = -1;
  Tenths tenths{};
  std::vector<Conversion> conversions;
};

// One item's options that can change the score. Clashes are kept as bit
// rows over the specials, then the stat options.
struct ItemOptions {
  bool present = false;
  // How many enchantments it holds here.
  int room = 0;
  std::vector<std::int32_t> specials;
  std::vector<StatOption> stats;
  // The most stat options that can share the item.
  int most_stats = 0;
  std::vector<std::vector<std::uint64_t>> clash;

  std::size_t Size() const { return specials.size() + stats.size(); }
  bool Clash(std::size_t a, std::size_t b) const { return (clash[a][b / 64] >> (b % 64)) & 1u; }
  std::size_t StatId(std::size_t i) const { return specials.size() + i; }
};

// A choice of options on one item as local ids, and what they add.
struct Pick {
  std::vector<std::size_t> ids;
  Tenths tenths{};
  std::vector<Conversion> conversions;
};

// Every clash-free set of the given options (local ids), up to `most` of
// them, the empty set first, each with what its stat options add.
void Sets(const ItemOptions& item, const std::vector<std::size_t>& ids, int most,
          const std::function<void(const Pick&)>& emit) {
  Pick pick;
  std::function<void(std::size_t)> grow = [&](std::size_t from) {
    emit(pick);
    if (std::cmp_greater_equal(pick.ids.size(), most)) {
      return;
    }
    for (std::size_t k = from; k < ids.size(); ++k) {
      const std::size_t id = ids[k];
      if (std::ranges::any_of(pick.ids, [&](std::size_t o) { return item.Clash(o, id); })) {
        continue;
      }
      pick.ids.push_back(id);
      const Tenths before = pick.tenths;
      const std::size_t conversions_before = pick.conversions.size();
      if (id >= item.specials.size()) {
        const StatOption& o = item.stats[id - item.specials.size()];
        for (std::size_t s = 0; s < kStats; ++s) pick.tenths[s] += o.tenths[s];
        pick.conversions.insert(pick.conversions.end(), o.conversions.begin(), o.conversions.end());
      }
      grow(k + 1);
      pick.ids.pop_back();
      pick.tenths = before;
      pick.conversions.resize(conversions_before);
    }
  };
  grow(0);
}

// Whether boost `a` is at least as strong as `x` in every way.
bool StrongerBoost(const Activate& a, const Activate& x) {
  const auto n = [](const Activate& b, std::string_view key, double fallback) { return b.Number(key).value_or(fallback); };
  return n(a, "amount", 0) >= n(x, "amount", 0) && n(a, "duration", 0) >= n(x, "duration", 0) &&
         n(a, "proc", 1) >= n(x, "proc", 1) && n(a, "cooldown", 0) <= n(x, "cooldown", 0);
}

bool NoStackChanceBoost(const Activate& a) {
  return IsChanceBoost(a) && a.Text(kChannel).starts_with("noStack");
}

// Whether mutator `a` does at least what `x` does, in a way that can't
// lower the score: higher damage and fire rate multipliers, a lower MP cost,
// a stronger chance boost of the same stat, more of a flat stat unless more
// of it can lower a read stat (`down`); anything else only when it's the
// same. That these never lower the score is an assumption, checked by the
// property tests and by AssumptionsFor's audit. `rivals` are the chance
// boosts that don't stack which any of the items can have.
bool AtLeast(const Mutator& a, const Mutator& x, const Mask& down, std::span<const Activate> rivals) {
  if (a.tag != x.tag || a.text != x.text) return false;
  const auto same_except = [&](std::initializer_list<std::string_view> keys) {
    for (const auto& [k, v] : x.attributes.params) {
      if (std::ranges::find(keys, std::string_view(k)) != keys.end()) continue;
      const auto it = a.attributes.params.find(k);
      if (it == a.attributes.params.end() || it->second != v) return false;
    }
    return a.attributes.params.size() == x.attributes.params.size();
  };
  const double na = a.number.value_or(1);
  const double nx = x.number.value_or(1);
  if (a.tag == "MultiplyMinDamage" || a.tag == "MultiplyMaxDamage" || a.tag == "MultiplyRateOfFire") {
    return na >= nx && same_except({});
  }
  if (a.tag == "MultiplyMPCost") {
    return na <= nx && same_except({});
  }
  const auto number = [](const Mutator& m, std::string_view key, double fallback) {
    return m.attributes.Number(key).value_or(fallback);
  };
  if (a.tag == "AddOnPlayerShootActivate" && a.text == "StatBoostSelf") {
    // Specials' boosts all have the gear they need, so what they need
    // doesn't matter.
    if (!StrongerBoost(a.attributes, x.attributes) ||
        !same_except({"amount", "duration", "proc", "cooldown", "mustWear", "mustWearOR", "mustNotWear",
                      "mustNotWearOR"})) {
      return false;
    }
    Activate boost = x.attributes;
    boost.type = x.text;
    if (!NoStackChanceBoost(boost)) return true;
    // Of chance boosts of a stat that don't stack, the engine counts one: the
    // highest by amount, then duration, then chance, the first on a tie
    // (CountsAsChanceBoost). Swapping `x` for `a` can make `a` count instead
    // of a rival ranked between them, so `a` must beat each of those too.
    const auto rank = [](const Activate& b) {
      return std::tuple{b.Number(kAmount).value_or(0), b.Number(kDuration).value_or(0), b.Number(kProc).value_or(0)};
    };
    return std::ranges::all_of(rivals, [&](const Activate& y) {
      return y.Text(kStat) != x.attributes.Text(kStat) || rank(y) < rank(x.attributes) ||
             rank(a.attributes) < rank(y) || StrongerBoost(a.attributes, y);
    });
  }
  if (a.tag == "ActivateOnEquip" && a.text == "IncrementStat") {
    const int stat = StatIndex(a.attributes.Text(kStat));
    const bool lowers = stat >= 0 && down[static_cast<std::size_t>(stat)];
    const double more = number(a, "amount", 0) - number(x, "amount", 0);
    return (lowers ? more == 0 : more >= 0) && same_except({"amount"});
  }
  return a.number == x.number && same_except({});
}

// Whether special `a`'s effects match `x`'s one for one, each at least as
// strong.
bool StrongerEffects(const Enchantment& a, const Enchantment& x, const Mask& down, std::span<const Activate> rivals) {
  if (a.mutators.size() != x.mutators.size()) return false;
  std::vector<bool> used(a.mutators.size(), false);
  for (const Mutator& mx : x.mutators) {
    bool matched = false;
    for (std::size_t i = 0; i < a.mutators.size() && !matched; ++i) {
      if (!used[i] && AtLeast(a.mutators[i], mx, down, rivals)) {
        used[i] = true;
        matched = true;
      }
    }
    if (!matched) return false;
  }
  return true;
}

// A clash-free set of flat options and its sum on the stats that matter.
struct FlatSet {
  std::vector<std::size_t> ids;
  Tenths tenths{};
};

// An item's flat sets, with the most and least of each stat over them.
struct FlatSets {
  std::vector<FlatSet> sets;
  Tenths most{};
  Tenths least{};
};

// A final the search reached: its stats and how it's made.
struct Final {
  Stats stats{};
  std::array<std::size_t, kSlots> conv{};
  std::array<const FlatSet*, kSlots> flat{};
};

// A sum of flat sets so far, and which set it took from each item.
template <std::size_t D>
struct Sum {
  Point<D> at{};
  std::array<std::uint16_t, kSlots> pick{};
};

// Whether one flat sum, as a point on the compared stats, ends at least as
// high as another on every read stat whatever is added to both. Comparing
// points needs a stat that moves read stats both ways to be equal (with
// Faded Heart Scarab more DEX is more DEX but less ATT); this lets more of
// it through when the rest makes up for it. A read stat is the floor of its
// flat sum plus the floor of each list's conversions into it (FinalStats).
// Adding the same to both sums, a floor of tenths/10 moves by at least the
// floor of the difference/10, and a stat a conversion reads by between the
// floor and the ceiling of it.
template <std::size_t D>
class Outdoes {
 public:
  Outdoes(const std::vector<std::pair<std::size_t, int>>& dims, const std::vector<std::size_t>& read,
          std::span<const Conversion> conversions, const StatBase& base) {
    std::array<int, kStats> plus;
    std::array<int, kStats> minus;
    plus.fill(-1);
    minus.fill(-1);
    for (std::size_t k = 0; k < dims.size(); ++k) {
      (dims[k].second > 0 ? plus : minus)[dims[k].first] = static_cast<int>(k);
    }
    // A stat's difference read from its coordinate. A stat not compared
    // moves no read stat.
    const auto coordinate = [&](std::size_t s) -> std::pair<std::size_t, int> {
      if (plus[s] >= 0) return {static_cast<std::size_t>(plus[s]), 1};
      return {static_cast<std::size_t>(std::max(minus[s], 0)), minus[s] >= 0 ? -1 : 0};
    };
    for (const std::size_t r : read) {
      Stat stat;
      std::tie(stat.k, std::ignore) = coordinate(r);
      stat.first = terms_.size();
      const auto add = [&](std::span<const Conversion> list, std::size_t which) {
        for (const Conversion& c : list) {
          if (c.to != r || c.percent == 0 || (!c.of_total && !base.has_class)) continue;
          // Shares in millionths: the data's percents are whole hundredths.
          const double millionths = c.percent * 1e4;
          const auto share = static_cast<std::int64_t>(std::llround(millionths));
          if (std::abs(millionths - static_cast<double>(share)) > 1e-6) exact_ = false;
          const auto [k, sign] = coordinate(c.from);
          if (sign != 0) terms_.push_back({k, sign, share, c.of_total, which});
        }
      };
      add(conversions, 0);
      add(base.own, 1);
      stat.last = terms_.size();
      stats_.push_back(stat);
    }
    // Read stats nothing converts into are the quickest to rule a point out.
    std::ranges::stable_sort(stats_, {}, [](const Stat& st) { return st.last - st.first; });
  }

  bool operator()(const Point<D>& x, const Point<D>& y) const {
    if (!exact_) return false;
    for (const Stat& st : stats_) {
      std::int32_t least = PointsOf(x[st.k] - y[st.k]);
      if (st.first == st.last) {
        if (least < 0) return false;
        continue;
      }
      std::array<std::int64_t, 2> moved{};
      for (std::size_t i = st.first; i < st.last; ++i) {
        const Term& t = terms_[i];
        const std::int32_t d = t.sign * (x[t.k] - y[t.k]);
        std::int32_t lo = PointsOf(d);
        std::int32_t hi = -PointsOf(-d);
        // A share of the bonus over the class max moves less, or not at all.
        if (!t.of_total) {
          lo = std::min(lo, 0);
          hi = std::max(hi, 0);
        }
        moved[t.list] += t.share * (t.share > 0 ? lo : hi);
      }
      for (const std::int64_t m : moved) least += static_cast<std::int32_t>(FloorDiv(m, 1'000'000));
      if (least < 0) return false;
    }
    return true;
  }

 private:
  struct Stat {
    std::size_t k = 0;
    std::size_t first = 0;
    std::size_t last = 0;
  };
  struct Term {
    std::size_t k;
    int sign;
    std::int64_t share;
    bool of_total;
    std::size_t list;
  };

  static std::int64_t FloorDiv(std::int64_t a, std::int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

  // False when a share isn't whole millionths; nothing is outdone then.
  bool exact_ = true;
  std::vector<Stat> stats_;
  std::vector<Term> terms_;
};

// Drops the items another kept one outdoes, the strongest by `order` first.
template <std::size_t D, typename T, typename Get, typename Order>
void DropOutdone(std::vector<T>& items, const Get& point, const Order& order, const Outdoes<D>& outdoes) {
  std::ranges::stable_sort(items, std::greater<>{}, order);
  std::size_t kept = 0;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const bool outdone = std::any_of(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(kept),
                                     [&](const T& k) { return outdoes(point(k), point(items[i])); });
    if (!outdone) items[kept++] = items[i];
  }
  items.resize(kept);
}

// Buffers a thread reuses from one merge to the next, so a merge allocates
// only while they grow.
template <std::size_t D>
struct MergeScratch {
  std::array<std::vector<std::pair<Point<D>, std::uint16_t>>, kSlots> own;
  std::vector<Sum<D>> sums;
  std::vector<Sum<D>> grown;
  std::vector<Stats> finals;
  std::vector<std::int32_t> rows;
  std::vector<std::uint32_t> kept;
  FrontierScratch frontier;

  // Pool threads live as long as the page, so a buffer past this size is
  // given back rather than kept for the next merge.
  void Trim() {
    constexpr std::size_t kMostBytes = std::size_t{1} << 20;
    const auto trim = [](auto& v) {
      if (v.capacity() * sizeof(v[0]) > kMostBytes) std::remove_reference_t<decltype(v)>().swap(v);
    };
    for (auto& v : own) trim(v);
    trim(sums);
    trim(grown);
    trim(finals);
    trim(rows);
    trim(kept);
    trim(frontier.order);
    trim(frontier.steps);
    trim(frontier.tails);
    trim(frontier.block_most);
  }
};

// Final stats already reached, asked "does any have at least these?" far
// more often than it grows: a snapshot index rebuilt by Settle, plus the
// few finals added since, checked one by one.
class Reached {
 public:
  virtual ~Reached() = default;
  virtual bool Covers(const Stats& stats) const = 0;
  virtual void Add(const Stats& stats) = 0;
  virtual void Settle() = 0;
};

template <std::size_t D>
class ReachedOn : public Reached {
 public:
  explicit ReachedOn(std::vector<std::size_t> read) : read_(std::move(read)) {}

  bool Covers(const Stats& stats) const override {
    const Point<D> p = Pick(stats);
    if (snapshot_ && snapshot_->Covers(p)) return true;
    return std::ranges::any_of(recent_, [&](const Point<D>& q) { return search::Covers(q, p); });
  }

  void Add(const Stats& stats) override { recent_.push_back(Pick(stats)); }

  void Settle() override {
    if (recent_.empty()) return;
    all_.insert(all_.end(), recent_.begin(), recent_.end());
    recent_.clear();
    snapshot_ = std::make_unique<SnapshotIndex<D>>(all_);
  }

 private:
  Point<D> Pick(const Stats& stats) const {
    Point<D> p;
    for (std::size_t k = 0; k < D; ++k) p[k] = stats[read_[k]];
    return p;
  }
  std::vector<std::size_t> read_;
  std::vector<Point<D>> all_;
  std::vector<Point<D>> recent_;
  std::unique_ptr<SnapshotIndex<D>> snapshot_;
};

std::unique_ptr<Reached> MakeReached(const std::vector<std::size_t>& read) {
  switch (read.size()) {
    case 1: return std::make_unique<ReachedOn<1>>(read);
    case 2: return std::make_unique<ReachedOn<2>>(read);
    case 3: return std::make_unique<ReachedOn<3>>(read);
    case 4: return std::make_unique<ReachedOn<4>>(read);
    case 5: return std::make_unique<ReachedOn<5>>(read);
    case 6: return std::make_unique<ReachedOn<6>>(read);
    case 7: return std::make_unique<ReachedOn<7>>(read);
    default: return std::make_unique<ReachedOn<8>>(read);
  }
}

class Search {
 public:
  Search(const Model& model, Scorer& scorer, const std::array<std::int32_t, kSlots>& items,
         const EnchantSettings& settings, SearchCounts& counts)
      : model_(model), scorer_(scorer), items_(items), settings_(settings), counts_(counts) {}

  std::optional<EnchantResult> Run(double incumbent);
  EnchantAssumptions Assume();

 private:
  struct Profile;
  struct Assignment;

  void FindObtainable();
  void FindReadStats();
  int UnreadStat(const PreparedLayout& layout) const;
  void CompileOptions();
  bool RangeCanMatter(const std::vector<std::int32_t>& range, const std::vector<std::int32_t>& shaping);
  const FlatSets& SetsOf(std::size_t slot, const std::vector<std::size_t>& open, int room);
  Profile& ProfileFor(const std::array<std::vector<std::size_t>, kSlots>& specials);
  void FindFinals(Profile& profile);
  void Directions(std::span<const Conversion> conversions, const PreparedLayout& layout, Mask& up,
                  Mask& down) const;
  // Read stats come first among the compared, and the strongest points
  // are those with the most of them.
  template <std::size_t D>
  std::int64_t Strength(const Point<D>& p) const {
    std::int64_t total = 0;
    for (std::size_t k = 0; k < read_list_.size(); ++k) total += p[k];
    return total;
  }
  template <std::size_t D>
  std::vector<Final> Merge(const Profile& profile, const Assignment& a, const Reached& reached) const;
  template <std::size_t D>
  const std::vector<std::uint16_t>& OwnFrontier(const std::vector<FlatSet>& sets, const Assignment& a,
                                                std::uint32_t signature) const;
  std::vector<Final> FinalsOf(const Profile& profile, const std::array<std::size_t, kSlots>& conv,
                              const Reached& reached) const;
  bool CanBeat(const PreparedLayout& layout, const std::vector<const Final*>& group, double best) const;
  std::size_t Widest(const std::vector<const Final*>& group) const;
  bool AnyBeats(const PreparedLayout& layout, std::vector<const Final*> group, double best) const;
  bool CutByOpen(const PreparedLayout& layout, const std::array<std::vector<std::size_t>, kSlots>& specials,
                 double best) const;
  void ScoreLayout(const PreparedLayout& layout, const std::array<std::vector<std::size_t>, kSlots>& specials);
  Build BuildOf(const Profile& profile, const std::array<std::vector<std::size_t>, kSlots>& specials,
                const Final& f) const;

  const Model& model_;
  Scorer& scorer_;
  std::array<std::int32_t, kSlots> items_;
  EnchantSettings settings_;
  SearchCounts& counts_;

  std::vector<int> worn_;
  // What each item can take, and how many it holds.
  std::array<std::vector<std::int32_t>, kSlots> obtainable_;
  std::array<int, kSlots> room_{};
  bool weapon_counts_ = true;
  Mask read_{};
  // The weapon's own reads, for on-shoot boosts.
  Mask boost_read_{};
  // Stats that matter: read, or converted from into a read stat.
  Mask useful_{};
  // Stats an item's own conversion takes a negative share of into a read
  // stat: more of them can lower the score, so they're only compared for
  // being equal.
  Mask down_{};
  std::vector<Activate> rivals_;
  std::vector<std::size_t> read_list_;
  std::array<ItemOptions, kSlots> options_;
  std::map<std::tuple<std::size_t, int, std::vector<std::size_t>>, std::unique_ptr<FlatSets>> flat_sets_;
  std::map<std::vector<std::int64_t>, std::unique_ptr<Profile>> profiles_;
  // Frontiers of an item's flat sets by the stats compared, shared by the
  // threads.
  mutable std::mutex own_mu_;
  mutable std::map<std::pair<const std::vector<FlatSet>*, std::uint32_t>, std::unique_ptr<std::vector<std::uint16_t>>>
      own_frontiers_;
  double best_ = 0;
  std::optional<EnchantResult> found_;
  // The finals of the layout with no specials, once found.
  const std::vector<Final>* open_finals_ = nullptr;
  EnchantAssumptions assumed_;
};

struct Search::Profile {
  // Per item: stat options open, room for them, and conversion choices
  // with the flat sets that go with each.
  std::array<std::vector<std::size_t>, kSlots> open;
  std::array<int, kSlots> room{};
  std::array<std::vector<Pick>, kSlots> conversions;
  std::array<std::vector<const FlatSets*>, kSlots> flats;
  // What the specials add.
  Tenths fixed{};
  std::vector<Conversion> fixed_conversions;
  const PreparedLayout* layout = nullptr;
  std::vector<Final> finals;
  bool found = false;
};

struct Search::Assignment {
  std::array<std::size_t, kSlots> conv{};
  Tenths fixed{};
  std::vector<Conversion> conversions;
  // The stats sums are compared on, read stats first, each with the way
  // more of it moves the read stats: +1 up, -1 down (an item's own negative
  // share of it). A stat that does both, like DEX with Faded Heart Scarab
  // (more DEX, less ATT), is there twice, so only sums equal in it compare.
  std::vector<std::pair<std::size_t, int>> dims;
  // Some stat moves read stats both ways.
  bool mixed = false;
  // A best completion's stats, with early dropping off when there's none.
  std::optional<Ceilings> ceilings;
};

void Search::FindReadStats() {
  const Dataset& d = model_.dataset();
  const Weapon& weapon = d.weapons[static_cast<std::size_t>(items_[kWeapon])];
  const Ability* ability = items_[kAbility] < 0 ? nullptr : &d.abilities[static_cast<std::size_t>(items_[kAbility])];
  const Gear* armor = items_[kArmor] < 0 ? nullptr : &d.armors[static_cast<std::size_t>(items_[kArmor])];
  const Gear* ring = items_[kRing] < 0 ? nullptr : &d.rings[static_cast<std::size_t>(items_[kRing])];
  const auto mark = [](Mask& m, std::string_view stat) {
    if (const int i = StatIndex(stat); i >= 0) m[static_cast<std::size_t>(i)] = true;
  };
  if (weapon_counts_) {
    mark(read_, "ATT");
    mark(read_, "DEX");
    mark(boost_read_, "ATT");
    mark(boost_read_, "DEX");
  }
  // The cast rate depends on WIS.
  if (ability != nullptr) mark(read_, "WIS");
  const auto note = [&](const Activate& a, bool on_shoot) {
    if (on_shoot && !weapon_counts_) return;
    mark(read_, a.Text(kScalingStat));
    if (on_shoot) mark(boost_read_, a.Text(kScalingStat));
  };
  for (const Activate& a : weapon.on_shoot) note(a, true);
  for (const Activate& a : weapon.on_ability) note(a, false);
  if (ability != nullptr) {
    for (const Activate& a : ability->activates) note(a, false);
    for (const Activate& a : ability->on_shoot) note(a, true);
    for (const Activate& a : ability->on_ability) note(a, false);
  }
  for (const Gear* g : {armor, ring}) {
    if (g == nullptr) continue;
    for (const Activate& a : g->on_shoot) note(a, true);
    for (const Activate& a : g->on_ability) note(a, false);
  }
  for (std::size_t s = 0; s < kSlots; ++s) {
    for (const std::int32_t e : obtainable_[s]) {
      for (const Mutator& m : model_.enchantment(e).mutators) {
        if (m.tag == "AddOnPlayerShootActivate") note(m.attributes, true);
      }
    }
  }
}

// A stat the search doesn't track that the layout's score, without the
// tie-break, moves with, or -1. Each is raised a little and a lot from the
// layout's own stats. Not a proof, a guard: it catches a formula that
// starts reading a new stat.
int Search::UnreadStat(const PreparedLayout& layout) const {
  const double base = scorer_.Weighted(layout, scorer_.WithStats(layout, layout.stats.before));
  for (std::size_t s = 0; s < kStats; ++s) {
    if (read_[s]) continue;
    for (const int more : {20, 200}) {
      Stats st = layout.stats.before;
      st[s] += more;
      const double v = scorer_.Weighted(layout, scorer_.WithStats(layout, st));
      if (std::abs(v - base) > 1e-9 * std::max(1.0, std::abs(base))) return static_cast<int>(s);
    }
  }
  return -1;
}

void Search::FindObtainable() {
  const Dataset& d = model_.dataset();
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    obtainable_[slot].clear();
    room_[slot] = 0;
    if (items_[slot] < 0) continue;
    const auto i = static_cast<std::size_t>(items_[slot]);
    const auto how = [&](const Enchantment& e) {
      switch (slot) {
        case kWeapon: return HowObtained(e, d.weapons[i], d);
        case kAbility: return HowObtained(e, d.abilities[i], d);
        case kArmor: return HowObtained(e, d.armors[i], d);
        default: return HowObtained(e, d.rings[i], d);
      }
    };
    const Enchanting& enchanting = slot == kWeapon    ? d.weapons[i].enchanting
                                   : slot == kAbility ? d.abilities[i].enchanting
                                   : slot == kArmor   ? d.armors[i].enchanting
                                                      : d.rings[i].enchanting;
    room_[slot] = std::min(settings_.per_item, EnchantmentSlots(enchanting, d));
    for (std::size_t e = 0; e < d.enchantments.size(); ++e) {
      const Obtained o = how(d.enchantments[e]);
      if (o == Obtained::kRolls || (o == Obtained::kArtifact && settings_.artifacts)) {
        obtainable_[slot].push_back(static_cast<std::int32_t>(e));
      }
    }
  }
}

void Search::CompileOptions() {
  // Conversions into read stats, and the stats they come from.
  Mask sources{};
  std::array<std::vector<std::pair<std::int32_t, bool>>, kSlots> relevant;
  // Whether a mutator does something the engine models that reaches the
  // score, beyond stats.
  const auto special = [&](std::size_t slot, const Mutator& m) {
    if (m.tag == "AddOnPlayerShootActivate") {
      const int stat = StatIndex(m.attributes.Text(kStat));
      const double proc = m.attributes.Number(kProc).value_or(1);
      if (m.text == "StatBoostSelf") {
        if (stat < 0 || !GearWorn(m.attributes, worn_)) return false;
        const auto st = static_cast<std::size_t>(stat);
        // A chance boost reaches the weapon only; a boost while firing
        // reaches the ability too.
        if (proc > 0 && proc < 1) return weapon_counts_ && boost_read_[st];
        return proc >= 1 && ((weapon_counts_ && boost_read_[st]) || (items_[kAbility] >= 0 && read_[st]));
      }
      return weapon_counts_ && (m.text == "PoisonGrenade" || m.text == "BulletCreate");
    }
    if (slot == kWeapon && (m.tag == "MultiplyMinDamage" || m.tag == "MultiplyMaxDamage" ||
                            m.tag == "MultiplyRateOfFire" || m.tag == "MultiplySpeed" ||
                            m.tag == "MultiplyLifetimeMS" || m.tag == "SetSubAttack" || m.tag == "AddSubAttack")) {
      return weapon_counts_;
    }
    return slot == kAbility && m.tag == "MultiplyMPCost";
  };
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    ItemOptions& item = options_[slot];
    item = {};
    if (items_[slot] < 0) continue;
    item.present = true;
    item.room = room_[slot];
    for (const std::int32_t id : obtainable_[slot]) {
      const auto& mutators = model_.enchantment(id).mutators;
      const bool is_special = std::ranges::any_of(mutators, [&](const Mutator& m) { return special(slot, m); });
      relevant[slot].emplace_back(id, is_special);
      for (const Conversion& c : model_.effect(id).conversions) {
        if (read_[c.to]) sources[c.from] = true;
      }
    }
  }
  // The items' own conversions read their sources too.
  for (std::size_t s = 0; s < kStats; ++s) useful_[s] = read_[s] || sources[s];
  down_ = {};
  rivals_.clear();
  if (const PreparedLayout* open = scorer_.Prepare(Layout{items_, {}})) {
    for (const Conversion& c : open->stats.own) {
      if (!read_[c.to]) continue;
      useful_[c.from] = true;
      if (c.percent < 0) down_[c.from] = true;
    }
    // The weapon's on-shoot effects, with the other items' joined.
    for (const Activate& a : open->request.weapon->on_shoot) {
      if (NoStackChanceBoost(a)) rivals_.push_back(a);
    }
  }
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    for (const std::int32_t id : obtainable_[slot]) {
      for (const Mutator& m : model_.enchantment(id).mutators) {
        if (m.tag != "AddOnPlayerShootActivate") continue;
        Activate a = m.attributes;
        a.type = m.text;
        if (NoStackChanceBoost(a)) rivals_.push_back(std::move(a));
      }
    }
  }
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    ItemOptions& item = options_[slot];
    if (!item.present) continue;
    std::vector<StatOption> stats;
    for (const auto& [id, is_special] : relevant[slot]) {
      const StatEffect& effect = model_.effect(id);
      if (is_special) {
        item.specials.push_back(id);
        continue;
      }
      StatOption o;
      o.enchantment = id;
      bool counts = false;
      for (std::size_t s = 0; s < kStats; ++s) {
        if (useful_[s] && effect.tenths[s] != 0) {
          o.tenths[s] = effect.tenths[s];
          counts = true;
        }
      }
      for (const Conversion& c : effect.conversions) {
        if (read_[c.to]) {
          o.conversions.push_back(c);
          counts = true;
        }
      }
      if (counts) {
        stats.push_back(std::move(o));
      } else {
        assumed_.ignored[slot].push_back(id);
      }
    }
    if (slot == kWeapon) {
      const auto ranged = [](const Mutator& m) { return m.tag == "MultiplySpeed" || m.tag == "MultiplyLifetimeMS"; };
      std::vector<std::int32_t> range;
      std::vector<std::int32_t> shaping;
      for (const std::int32_t id : item.specials) {
        const auto& mutators = model_.enchantment(id).mutators;
        if (std::ranges::all_of(mutators, ranged)) {
          range.push_back(id);
        } else if (std::ranges::any_of(mutators, [&](const Mutator& m) {
                     return ranged(m) || m.tag == "SetSubAttack" || m.tag == "AddSubAttack";
                   })) {
          shaping.push_back(id);
        }
      }
      if (!range.empty() && !RangeCanMatter(range, shaping)) {
        std::erase_if(item.specials, [&](std::int32_t id) { return std::ranges::find(range, id) != range.end(); });
        assumed_.ignored[slot].insert(assumed_.ignored[slot].end(), range.begin(), range.end());
      }
    }
    // A special is never needed when another can take its place (they
    // can't share the item, and the other clashes with nothing it doesn't)
    // and does at least as much. Equal ones keep the first.
    {
      const auto covers_clashes = [&](std::int32_t a, std::int32_t x) {
        const Enchantment& ea = model_.enchantment(a);
        const Enchantment& ex = model_.enchantment(x);
        if (!Conflict(ea, ex)) return false;
        for (const std::int32_t o : obtainable_[slot]) {
          if (o != a && o != x && Conflict(ea, model_.enchantment(o)) && !Conflict(ex, model_.enchantment(o))) {
            return false;
          }
        }
        return true;
      };
      std::vector<std::int32_t> kept;
      for (const std::int32_t x : item.specials) {
        const auto by = std::ranges::find_if(item.specials, [&](std::int32_t a) {
          if (a == x || !StrongerEffects(model_.enchantment(a), model_.enchantment(x), down_, rivals_)) return false;
          const bool equal = StrongerEffects(model_.enchantment(x), model_.enchantment(a), down_, rivals_);
          return (!equal || a < x) && covers_clashes(a, x);
        });
        if (by == item.specials.end()) {
          kept.push_back(x);
        } else {
          assumed_.replaced[slot].emplace_back(x, *by);
        }
      }
      item.specials = std::move(kept);
    }
    // Clashes between every option of the item, specials first.
    std::vector<std::int32_t> all = item.specials;
    for (const StatOption& o : stats) all.push_back(o.enchantment);
    const std::size_t words = (all.size() + 63) / 64;
    std::vector<std::vector<std::uint64_t>> clash(all.size(), std::vector<std::uint64_t>(words, 0));
    for (std::size_t a = 0; a < all.size(); ++a) {
      for (std::size_t b = 0; b < all.size(); ++b) {
        if (a == b || Conflict(model_.enchantment(all[a]), model_.enchantment(all[b]))) {
          clash[a][b / 64] |= std::uint64_t{1} << (b % 64);
        }
      }
    }
    // A stat option is never needed when another can take its place (they
    // can't share the item, and the other clashes with nothing it doesn't)
    // and adds at least as much of every stat that matters and every
    // conversion. Equal ones keep the first.
    const std::size_t first_stat = item.specials.size();
    const auto replaces = [&](std::size_t a, std::size_t x) {
      if (a == x) return false;
      const auto ia = first_stat + a;
      const auto ix = first_stat + x;
      if (!((clash[ia][ix / 64] >> (ix % 64)) & 1u)) return false;
      for (std::size_t w = 0; w < words; ++w) {
        std::uint64_t extra = clash[ia][w] & ~clash[ix][w];
        if (w == ia / 64) extra &= ~(std::uint64_t{1} << (ia % 64));
        if (extra != 0) return false;
      }
      const StatOption& oa = stats[a];
      const StatOption& ox = stats[x];
      bool more = false;
      for (std::size_t s = 0; s < kStats; ++s) {
        if (oa.tenths[s] < ox.tenths[s] || (down_[s] && oa.tenths[s] != ox.tenths[s])) return false;
        more = more || oa.tenths[s] > ox.tenths[s];
      }
      if (oa.conversions.size() != ox.conversions.size()) return false;
      for (const Conversion& cx : ox.conversions) {
        const auto ca = std::ranges::find_if(oa.conversions, [&](const Conversion& c) {
          return c.from == cx.from && c.to == cx.to && c.of_total == cx.of_total;
        });
        if (ca == oa.conversions.end() || ca->percent < cx.percent) return false;
        more = more || ca->percent > cx.percent;
      }
      return more || a < x;
    };
    std::vector<std::size_t> keep;
    for (std::size_t x = 0; x < stats.size(); ++x) {
      std::size_t by = stats.size();
      for (std::size_t a = 0; a < stats.size() && by == stats.size(); ++a) {
        if (replaces(a, x)) by = a;
      }
      if (by == stats.size()) {
        keep.push_back(x);
      } else {
        assumed_.replaced[slot].emplace_back(stats[x].enchantment, stats[by].enchantment);
      }
    }
    for (const std::size_t x : keep) item.stats.push_back(std::move(stats[x]));
    // The clash rows again over what's left.
    std::vector<std::size_t> rows;
    for (std::size_t i = 0; i < first_stat; ++i) rows.push_back(i);
    for (const std::size_t x : keep) rows.push_back(first_stat + x);
    const std::size_t kept_words = (rows.size() + 63) / 64;
    item.clash.assign(rows.size(), std::vector<std::uint64_t>(kept_words, 0));
    for (std::size_t a = 0; a < rows.size(); ++a) {
      for (std::size_t b = 0; b < rows.size(); ++b) {
        if ((clash[rows[a]][rows[b] / 64] >> (rows[b] % 64)) & 1u) {
          item.clash[a][b / 64] |= std::uint64_t{1} << (b % 64);
        }
      }
    }
    std::vector<std::size_t> stat_ids;
    for (std::size_t i = 0; i < item.stats.size(); ++i) stat_ids.push_back(item.StatId(i));
    Sets(item, stat_ids, item.room, [&](const Pick& p) {
      item.most_stats = std::max(item.most_stats, static_cast<int>(p.ids.size()));
    });
  }
}

// Whether a weapon special that only changes projectile speed or lifetime
// (`range`) can change the score. The engine reads those only through the
// hit estimate (HitsPerVolley), so it can't when, with every clash-free set
// of the specials that change the weapon's shots (`shaping`), every set of
// `range` leaves each subattack's hits per volley as they were.
bool Search::RangeCanMatter(const std::vector<std::int32_t>& range, const std::vector<std::int32_t>& shaping) {
  const Assumptions& assumptions = model_.base().assumptions;
  if (!assumptions.distance) return false;
  const auto hits = [&](std::vector<std::int32_t> on_weapon) -> std::optional<std::vector<int>> {
    Layout key;
    key.items = items_;
    std::ranges::sort(on_weapon);
    key.specials[kWeapon] = std::move(on_weapon);
    const PreparedLayout* layout = scorer_.Prepare(key);
    if (layout == nullptr) return std::nullopt;
    const Weapon& w = *layout->request.weapon;
    std::vector<int> out;
    for (const Subattack& s : w.subattacks) {
      out.push_back(HitsPerVolley(w, s, *assumptions.distance, assumptions.target_radius));
    }
    return out;
  };
  const auto fits = [&](const std::vector<std::int32_t>& set, std::int32_t id) {
    return std::cmp_less(set.size(), room_[kWeapon]) && std::ranges::none_of(set, [&](std::int32_t o) {
             return Conflict(model_.enchantment(o), model_.enchantment(id));
           });
  };
  // Past this many sets, it's assumed to matter rather than checked.
  constexpr std::size_t kMostTried = 4096;
  std::size_t tried = 0;
  bool matters = false;
  std::vector<std::int32_t> set;
  std::function<void(std::size_t, const std::vector<int>&)> add_range = [&](std::size_t from,
                                                                             const std::vector<int>& before) {
    for (std::size_t k = from; k < range.size() && !matters; ++k) {
      if (!fits(set, range[k])) continue;
      set.push_back(range[k]);
      const auto now = hits(set);
      matters = ++tried > kMostTried || (now && *now != before);
      add_range(k + 1, before);
      set.pop_back();
    }
  };
  std::function<void(std::size_t)> add_shaping = [&](std::size_t from) {
    if (const auto before = hits(set)) add_range(0, *before);
    for (std::size_t k = from; k < shaping.size() && !matters; ++k) {
      if (!fits(set, shaping[k])) continue;
      set.push_back(shaping[k]);
      add_shaping(k + 1);
      set.pop_back();
    }
  };
  add_shaping(0);
  return matters;
}

const FlatSets& Search::SetsOf(std::size_t slot, const std::vector<std::size_t>& open, int room) {
  auto key = std::tuple{slot, room, open};
  if (auto it = flat_sets_.find(key); it != flat_sets_.end()) {
    return *it->second;
  }
  const ItemOptions& item = options_[slot];
  std::vector<FlatSet> sets;
  Sets(item, open, room, [&](const Pick& p) { sets.push_back({p.ids, p.tenths}); });
  // Only sets nothing else matches or beats on the stats that matter.
  // A stat more of which can lower the score counts both ways, so only sets
  // equal in it compare.
  std::vector<std::pair<std::size_t, std::int32_t>> dims;
  for (std::size_t s = 0; s < kStats; ++s) {
    if (useful_[s]) dims.emplace_back(s, 1);
  }
  for (std::size_t s = 0; s < kStats; ++s) {
    if (down_[s]) dims.emplace_back(s, -1);
  }
  std::vector<std::int32_t> rows;
  rows.reserve(sets.size() * dims.size());
  for (const FlatSet& set : sets) {
    for (const auto& [stat, sign] : dims) rows.push_back(sign * set.tenths[stat]);
  }
  auto kept = std::make_unique<FlatSets>();
  kept->most.fill(std::numeric_limits<std::int32_t>::min() / 4);
  kept->least.fill(std::numeric_limits<std::int32_t>::max() / 4);
  for (const std::size_t i : FrontierOfRows(rows, dims.size())) {
    for (std::size_t k = 0; k < kStats; ++k) {
      kept->most[k] = std::max(kept->most[k], sets[i].tenths[k]);
      kept->least[k] = std::min(kept->least[k], sets[i].tenths[k]);
    }
    kept->sets.push_back(std::move(sets[i]));
  }
  return *flat_sets_.emplace(std::move(key), std::move(kept)).first->second;
}

Search::Profile& Search::ProfileFor(const std::array<std::vector<std::size_t>, kSlots>& specials) {
  // What decides the finals: per item the stat options left open and the
  // room for them, and what the specials add.
  std::vector<std::int64_t> key;
  Profile p;
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    const ItemOptions& item = options_[slot];
    if (!item.present) {
      key.push_back(-1);
      continue;
    }
    for (std::size_t i = 0; i < item.stats.size(); ++i) {
      const std::size_t id = item.StatId(i);
      if (std::ranges::none_of(specials[slot], [&](std::size_t sp) { return item.Clash(sp, id); })) {
        p.open[slot].push_back(id);
      }
    }
    const int left = item.room - static_cast<int>(specials[slot].size());
    p.room[slot] = std::max(0, std::min(left, item.most_stats));
    key.push_back(p.room[slot]);
    key.push_back(static_cast<std::int64_t>(p.open[slot].size()));
    for (const std::size_t id : p.open[slot]) key.push_back(static_cast<std::int64_t>(id));
    for (const std::size_t sp : specials[slot]) {
      const StatEffect& e = model_.effect(item.specials[sp]);
      for (std::size_t s = 0; s < kStats; ++s) p.fixed[s] += e.tenths[s];
      for (const Conversion& c : e.conversions) {
        if (read_[c.to]) p.fixed_conversions.push_back(c);
      }
    }
  }
  for (std::size_t s = 0; s < kStats; ++s) key.push_back(p.fixed[s]);
  for (const Conversion& c : p.fixed_conversions) {
    key.push_back(c.from * 16 + c.to);
    key.push_back(std::llround(c.percent * 1e6));
  }
  auto it = profiles_.find(key);
  if (it == profiles_.end()) {
    it = profiles_.emplace(std::move(key), std::make_unique<Profile>(std::move(p))).first;
  }
  return *it->second;
}

// How more of each stat moves the read stats under these conversions: up
// when it's read or converts into a read stat, down when an item's own
// conversion takes a negative share of it into one.
void Search::Directions(std::span<const Conversion> conversions, const PreparedLayout& layout, Mask& up,
                        Mask& down) const {
  up = read_;
  down = {};
  for (const Conversion& c : conversions) {
    if (read_[c.to]) (c.percent >= 0 ? up : down)[c.from] = true;
  }
  for (const Conversion& c : layout.stats.own) {
    if (!read_[c.to] || c.percent == 0) continue;
    (c.percent > 0 ? up : down)[c.from] = true;
  }
}

void Search::FindFinals(Profile& profile) {
  profile.found = true;
  ++counts_.profiles;
  const PreparedLayout& layout = *profile.layout;
  static const FlatSets kNothing = {{FlatSet{}}, {}, {}};
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    const ItemOptions& item = options_[slot];
    if (!item.present || profile.room[slot] == 0) {
      profile.conversions[slot] = {Pick{}};
      profile.flats[slot] = {&kNothing};
      continue;
    }
    std::vector<std::size_t> converting;
    std::vector<std::size_t> flat;
    for (const std::size_t id : profile.open[slot]) {
      (item.stats[id - item.specials.size()].conversions.empty() ? flat : converting).push_back(id);
    }
    Sets(item, converting, profile.room[slot], [&](const Pick& pick) { profile.conversions[slot].push_back(pick); });
    for (const Pick& pick : profile.conversions[slot]) {
      std::vector<std::size_t> open;
      for (const std::size_t id : flat) {
        if (std::ranges::none_of(pick.ids, [&](std::size_t o) { return item.Clash(o, id); })) open.push_back(id);
      }
      profile.flats[slot].push_back(&SetsOf(slot, open, profile.room[slot] - static_cast<int>(pick.ids.size())));
    }
  }
  // Items that can swap conversion choices without changing anything: the
  // same choices, and the same flat sets whichever non-empty choice they
  // take. Their choices are taken in order, as a multiset.
  std::array<int, kSlots> swap_class{};
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    swap_class[slot] = -1;
    const auto& flats = profile.flats[slot];
    const bool alike = flats.size() > 1 && std::ranges::all_of(flats.begin() + 1, flats.end(), [&](const auto* f) {
                         return f == flats[1];
                       });
    if (!alike) continue;
    const auto same_choices = [&](std::size_t other) {
      const auto& a = profile.conversions[slot];
      const auto& b = profile.conversions[other];
      if (a.size() != b.size()) return false;
      for (std::size_t k = 0; k < a.size(); ++k) {
        if (a[k].ids.size() != b[k].ids.size()) return false;
        for (std::size_t j = 0; j < a[k].ids.size(); ++j) {
          const auto ea = options_[slot].stats[a[k].ids[j] - options_[slot].specials.size()].enchantment;
          const auto eb = options_[other].stats[b[k].ids[j] - options_[other].specials.size()].enchantment;
          if (ea != eb) return false;
        }
      }
      return true;
    };
    for (std::size_t other = 0; other < slot && swap_class[slot] < 0; ++other) {
      if (swap_class[other] >= 0 && same_choices(other)) swap_class[slot] = swap_class[other];
    }
    if (swap_class[slot] < 0) swap_class[slot] = static_cast<int>(slot);
  }

  // Every assignment of conversion choices, with the best its sums could
  // reach, best first so strong finals come early and drop the rest.
  struct Planned {
    std::array<std::size_t, kSlots> conv;
    Stats best;
    std::int64_t order;
    // False when some stat moves one read stat both ways: no best case then.
    bool bounded;
  };
  std::vector<Planned> planned;
  std::array<std::size_t, kSlots> conv{};
  std::vector<Conversion> conversions;
  while (true) {
    bool canonical = true;
    for (std::size_t a = 0; a < kSlots && canonical; ++a) {
      for (std::size_t b = a + 1; b < kSlots && canonical; ++b) {
        if (swap_class[a] >= 0 && swap_class[a] == swap_class[b] && conv[a] > 0 && conv[b] > 0) {
          canonical = conv[a] <= conv[b];
        }
      }
    }
    if (canonical) {
      conversions = profile.fixed_conversions;
      for (std::size_t slot = 0; slot < kSlots; ++slot) {
        const Pick& pick = profile.conversions[slot][conv[slot]];
        conversions.insert(conversions.end(), pick.conversions.begin(), pick.conversions.end());
      }
      const Ceilings ceilings(read_list_, read_, conversions, layout.stats);
      // Each read stat at its best: the most of every stat that raises it,
      // the least of every stat that lowers it.
      Tenths most = profile.fixed;
      Tenths least = profile.fixed;
      for (std::size_t slot = 0; slot < kSlots; ++slot) {
        const Pick& pick = profile.conversions[slot][conv[slot]];
        const FlatSets& sets = *profile.flats[slot][conv[slot]];
        for (std::size_t s = 0; s < kStats; ++s) {
          most[s] += pick.tenths[s] + sets.most[s];
          least[s] += pick.tenths[s] + sets.least[s];
        }
      }
      Planned plan{conv, ceilings.Of(most, least, conversions), 0, ceilings.bounded()};
      for (const std::size_t s : read_list_) plan.order += plan.best[s];
      planned.push_back(plan);
    }
    std::size_t slot = 0;
    for (; slot < kSlots; ++slot) {
      if (++conv[slot] < profile.conversions[slot].size()) break;
      conv[slot] = 0;
    }
    if (slot == kSlots) break;
  }
  std::ranges::stable_sort(planned, std::greater<>{}, &Planned::order);

  // Assignments go in rounds. Each round's run at once, every one checked
  // against the finals reached before the round, so what each finds
  // doesn't depend on the threads; the round's finals then join in order.
  constexpr std::size_t kRound = 256;
  const std::unique_ptr<Reached> reached = MakeReached(read_list_);
  std::size_t next = 0;
  while (next < planned.size()) {
    std::vector<std::size_t> round;
    for (; next < planned.size() && round.size() < kRound; ++next) {
      if (!(planned[next].bounded && reached->Covers(planned[next].best))) round.push_back(next);
    }
    std::vector<std::vector<Final>> found(round.size());
    ParallelFor(round.size(), [&](std::size_t i) { found[i] = FinalsOf(profile, planned[round[i]].conv, *reached); });
    for (std::vector<Final>& list : found) {
      for (const Final& f : list) {
        if (!reached->Covers(f.stats)) {
          reached->Add(f.stats);
          profile.finals.push_back(f);
        }
      }
    }
    reached->Settle();
    counts_.assignments += round.size();
  }
  // Finals added early may be beaten by later ones.
  std::vector<std::int32_t> rows;
  for (const Final& f : profile.finals) {
    for (const std::size_t s : read_list_) rows.push_back(f.stats[s]);
  }
  std::vector<Final> kept;
  for (const std::size_t i : FrontierOfRows(rows, read_list_.size())) kept.push_back(profile.finals[i]);
  profile.finals = std::move(kept);
  counts_.finals += profile.finals.size();
}

std::vector<Final> Search::FinalsOf(const Profile& profile, const std::array<std::size_t, kSlots>& conv,
                                   const Reached& reached) const {
  const PreparedLayout& layout = *profile.layout;
  Assignment a;
  a.conv = conv;
  a.fixed = profile.fixed;
  a.conversions = profile.fixed_conversions;
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    const Pick& pick = profile.conversions[slot][conv[slot]];
    for (std::size_t s = 0; s < kStats; ++s) a.fixed[s] += pick.tenths[s];
    a.conversions.insert(a.conversions.end(), pick.conversions.begin(), pick.conversions.end());
  }
  Mask up{};
  Mask down{};
  Directions(a.conversions, layout, up, down);
  for (const std::size_t s : read_list_) a.dims.emplace_back(s, 1);
  for (std::size_t s = 0; s < kStats; ++s) {
    if (up[s] && !read_[s]) a.dims.emplace_back(s, 1);
  }
  for (std::size_t s = 0; s < kStats; ++s) {
    if (down[s]) a.dims.emplace_back(s, -1);
    a.mixed = a.mixed || (up[s] && down[s]);
  }
  a.ceilings.emplace(read_list_, read_, a.conversions, layout.stats);
  switch (a.dims.size()) {
    case 1: return Merge<1>(profile, a, reached);
    case 2: return Merge<2>(profile, a, reached);
    case 3: return Merge<3>(profile, a, reached);
    case 4: return Merge<4>(profile, a, reached);
    case 5: return Merge<5>(profile, a, reached);
    case 6: return Merge<6>(profile, a, reached);
    case 7: return Merge<7>(profile, a, reached);
    case 8: return Merge<8>(profile, a, reached);
    case 9: return Merge<9>(profile, a, reached);
    case 10: return Merge<10>(profile, a, reached);
    case 11: return Merge<11>(profile, a, reached);
    case 12: return Merge<12>(profile, a, reached);
    default:
      throw std::runtime_error("too many stats to compare: " + std::to_string(a.dims.size()));
  }
}

template <std::size_t D>
const std::vector<std::uint16_t>& Search::OwnFrontier(const std::vector<FlatSet>& sets, const Assignment& a,
                                                       std::uint32_t signature) const {
  const auto key = std::pair{&sets, signature};
  {
    std::lock_guard lock(own_mu_);
    if (auto it = own_frontiers_.find(key); it != own_frontiers_.end()) return *it->second;
  }
  std::vector<Point<D>> points;
  points.reserve(sets.size());
  for (const FlatSet& f : sets) {
    Point<D> p{};
    for (std::size_t k = 0; k < D; ++k) p[k] = a.dims[k].second * f.tenths[a.dims[k].first];
    points.push_back(p);
  }
  auto kept = std::make_unique<std::vector<std::uint16_t>>();
  for (const std::size_t i : Frontier(points)) kept->push_back(static_cast<std::uint16_t>(i));
  std::lock_guard lock(own_mu_);
  return *own_frontiers_.try_emplace(key, std::move(kept)).first->second;
}

template <std::size_t D>
std::vector<Final> Search::Merge(const Profile& profile, const Assignment& a, const Reached& reached) const {
  static_assert(D <= kMaxCompared);
  const PreparedLayout& layout = *profile.layout;
  thread_local MergeScratch<D> scratch;
  // An item's flat set as a point: its tenths on each compared stat,
  // negated where more of the stat lowers the read stats.
  const auto point_of = [&](const FlatSet& f) {
    Point<D> p{};
    for (std::size_t k = 0; k < D; ++k) p[k] = a.dims[k].second * f.tenths[a.dims[k].first];
    return p;
  };
  // Each item's sets that nothing else of its own matches or beats on
  // these stats. They depend only on the item's sets and the stats, so
  // they're kept for other assignments. With a stat compared both ways,
  // those that another outdoes go too.
  std::uint32_t signature = 0;
  for (const auto& [stat, sign] : a.dims) signature |= 1u << (stat + (sign < 0 ? kStats : 0));
  std::optional<Outdoes<D>> outdoes;
  if (a.mixed) outdoes.emplace(a.dims, read_list_, a.conversions, layout.stats);
  auto& own = scratch.own;
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    own[slot].clear();
    const std::vector<FlatSet>& sets = profile.flats[slot][a.conv[slot]]->sets;
    for (const std::uint16_t i : OwnFrontier<D>(sets, a, signature)) own[slot].emplace_back(point_of(sets[i]), i);
    if (outdoes) {
      DropOutdone(
          own[slot], [](const auto& o) -> const Point<D>& { return o.first; },
          [&](const auto& o) { return Strength(o.first); }, *outdoes);
    }
  }
  // The most the items after each can add to each compared stat.
  std::array<Point<D>, kSlots> rest{};
  for (std::size_t slot = kSlots - 1; slot > 0; --slot) {
    rest[slot - 1] = rest[slot];
    for (std::size_t k = 0; k < D; ++k) {
      std::int32_t top = std::numeric_limits<std::int32_t>::min() / 4;
      for (const auto& [p, i] : own[slot]) top = std::max(top, p[k]);
      rest[slot - 1][k] += top;
    }
  }
  // A stat compared both ways is read from its plain coordinate.
  const auto final_of = [&](const Point<D>& at) {
    Tenths tenths = a.fixed;
    Mask taken{};
    for (std::size_t k = 0; k < D; ++k) {
      const auto [s, sign] = a.dims[k];
      if (sign > 0) {
        tenths[s] += at[k];
        taken[s] = true;
      }
    }
    for (std::size_t k = 0; k < D; ++k) {
      const auto [s, sign] = a.dims[k];
      if (sign < 0 && !taken[s]) tenths[s] -= at[k];
    }
    return scorer_.FinalStats(layout, tenths, a.conversions);
  };
  // A sum whose best completion is matched by a final already reached.
  const auto hopeless = [&](const Point<D>& at, std::size_t slot) {
    if (!a.ceilings->bounded()) return false;
    Point<D> best = at;
    for (std::size_t k = 0; k < D; ++k) best[k] += rest[slot][k];
    if (!a.mixed) return reached.Covers(final_of(best));
    // A stat compared both ways has its most on one coordinate and its
    // least, negated, on the other.
    Tenths most = a.fixed;
    Tenths least = a.fixed;
    Mask taken{};
    for (std::size_t k = 0; k < D; ++k) {
      const auto [s, sign] = a.dims[k];
      if (sign > 0) {
        most[s] += best[k];
        least[s] += best[k];
        taken[s] = true;
      }
    }
    for (std::size_t k = 0; k < D; ++k) {
      const auto [s, sign] = a.dims[k];
      if (sign > 0) continue;
      least[s] = a.fixed[s] - best[k];
      if (!taken[s]) most[s] = least[s];
    }
    return reached.Covers(a.ceilings->Of(most, least, a.conversions));
  };
  // The most of every coordinate over a range.
  const auto corner = [](auto first, auto last, auto point) {
    Point<D> c;
    c.fill(std::numeric_limits<std::int32_t>::min() / 4);
    for (auto it = first; it != last; ++it) {
      for (std::size_t k = 0; k < D; ++k) c[k] = std::max(c[k], point(*it)[k]);
    }
    return c;
  };
  std::vector<Final> out;
  std::vector<Sum<D>>& sums = scratch.sums;
  sums.assign(1, Sum<D>{});
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    std::vector<Sum<D>>& grown = scratch.grown;
    grown.clear();
    // Sums and the item's sets in blocks of nearby points. Pairs from two
    // blocks are all hopeless when the blocks' corners add to a hopeless
    // point, since the bound only grows; then each sum with the sets'
    // corner; then each pair.
    constexpr std::size_t kSumBlock = 16;
    constexpr std::size_t kSetBlock = 8;
    auto& sets = own[slot];
    std::ranges::sort(sets, std::greater<>{}, [](const auto& o) { return o.first; });
    std::ranges::sort(sums, std::greater<>{}, [](const Sum<D>& x) { return x.at; });
    for (std::size_t ob = 0; ob < sets.size(); ob += kSetBlock) {
      const auto oe = std::min(sets.size(), ob + kSetBlock);
      const Point<D> sets_corner = corner(sets.begin() + static_cast<std::ptrdiff_t>(ob),
                                          sets.begin() + static_cast<std::ptrdiff_t>(oe),
                                          [](const auto& o) -> const Point<D>& { return o.first; });
      for (std::size_t sb = 0; sb < sums.size(); sb += kSumBlock) {
        const auto se = std::min(sums.size(), sb + kSumBlock);
        Point<D> both = corner(sums.begin() + static_cast<std::ptrdiff_t>(sb),
                               sums.begin() + static_cast<std::ptrdiff_t>(se),
                               [](const Sum<D>& x) -> const Point<D>& { return x.at; });
        for (std::size_t k = 0; k < D; ++k) both[k] += sets_corner[k];
        if (hopeless(both, slot)) continue;
        for (std::size_t xi = sb; xi < se; ++xi) {
          const Sum<D>& x = sums[xi];
          Point<D> with_corner = x.at;
          for (std::size_t k = 0; k < D; ++k) with_corner[k] += sets_corner[k];
          if (hopeless(with_corner, slot)) continue;
          for (std::size_t oi = ob; oi < oe; ++oi) {
            Sum<D> next = x;
            for (std::size_t k = 0; k < D; ++k) next.at[k] += sets[oi].first[k];
            next.pick[slot] = sets[oi].second;
            if (!hopeless(next.at, slot)) grown.push_back(next);
          }
        }
      }
    }
    if (slot + 1 < kSlots) {
      // Before the last item, a long list goes on as it is: comparing its
      // sums with each other costs more than checking each one's finals
      // against those already reached, which the last item does anyway.
      // Past about 20,000 sums the time hardly changes (measured on Knight
      // with Neo Acidic Slasher and Brutal Buckler); below 5,000 skipping
      // costs more than it saves.
      constexpr std::size_t kMostCompared = 50'000;
      if (slot + 2 == kSlots && grown.size() > kMostCompared) {
        sums = grown;
        continue;
      }
      FrontierInto<D>(grown.size(), [&](std::uint32_t i) -> const Point<D>& { return grown[i].at; }, scratch.frontier,
                      scratch.kept);
      sums.clear();
      for (const std::uint32_t i : scratch.kept) sums.push_back(grown[i]);
      if (outdoes) {
        DropOutdone(
            sums, [](const Sum<D>& x) -> const Point<D>& { return x.at; },
            [&](const Sum<D>& x) { return Strength(x.at); }, *outdoes);
      }
      continue;
    }
    // Nothing comes after the last item: compare final stats.
    std::vector<Stats>& finals = scratch.finals;
    std::vector<std::int32_t>& rows = scratch.rows;
    finals.clear();
    rows.clear();
    for (const Sum<D>& x : grown) {
      finals.push_back(final_of(x.at));
      for (const std::size_t s : read_list_) rows.push_back(finals.back()[s]);
    }
    FrontierOfRowsInto(rows, read_list_.size(), scratch.frontier, scratch.kept);
    for (const std::uint32_t i : scratch.kept) {
      if (reached.Covers(finals[i])) continue;
      Final f;
      f.stats = finals[i];
      f.conv = a.conv;
      for (std::size_t s = 0; s < kSlots; ++s) {
        f.flat[s] = &profile.flats[s][a.conv[s]]->sets[grown[i].pick[s]];
      }
      out.push_back(f);
    }
  }
  scratch.Trim();
  return out;
}

// Whether a build among the group's finals could score above `best`: the
// score at the most of every read stat over the group bounds them all.
bool Search::CanBeat(const PreparedLayout& layout, const std::vector<const Final*>& group, double best) const {
  Stats top = layout.stats.before;
  for (const std::size_t s : read_list_) {
    top[s] = std::numeric_limits<int>::min();
    for (const Final* f : group) top[s] = std::max(top[s], f->stats[s]);
  }
  return scorer_.Score(layout, scorer_.WithStats(layout, top)) > best;
}

Build Search::BuildOf(const Profile& profile, const std::array<std::vector<std::size_t>, kSlots>& specials,
                      const Final& f) const {
  Build b;
  b.items = items_;
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    const ItemOptions& item = options_[slot];
    if (!item.present) continue;
    for (const std::size_t sp : specials[slot]) b.enchants[slot].push_back(item.specials[sp]);
    const auto add = [&](std::size_t id) { b.enchants[slot].push_back(item.stats[id - item.specials.size()].enchantment); };
    for (const std::size_t id : profile.conversions[slot][f.conv[slot]].ids) add(id);
    for (const std::size_t id : f.flat[slot]->ids) add(id);
  }
  return b;
}

// The read stat the group's finals spread over most.
std::size_t Search::Widest(const std::vector<const Final*>& group) const {
  std::size_t widest = read_list_[0];
  int spread = -1;
  for (const std::size_t s : read_list_) {
    int lo = std::numeric_limits<int>::max();
    int hi = std::numeric_limits<int>::min();
    for (const Final* f : group) {
      lo = std::min(lo, f->stats[s]);
      hi = std::max(hi, f->stats[s]);
    }
    if (hi - lo > spread) {
      spread = hi - lo;
      widest = s;
    }
  }
  return widest;
}

// Whether any of the finals scores above `best` on this layout, splitting
// the group while its ceiling allows.
bool Search::AnyBeats(const PreparedLayout& layout, std::vector<const Final*> group, double best) const {
  if (group.empty() || !CanBeat(layout, group, best)) return false;
  if (group.size() == 1) return true;
  const std::size_t widest = Widest(group);
  std::ranges::stable_sort(group, {}, [&](const Final* f) { return f->stats[widest]; });
  const auto mid = static_cast<std::ptrdiff_t>(group.size() / 2);
  return AnyBeats(layout, {group.begin() + mid, group.end()}, best) ||
         AnyBeats(layout, {group.begin(), group.begin() + mid}, best);
}

// Whether the open layout's finals show this layout can't beat `best`. A
// layout whose specials add no stats reaches no stats the open layout (no
// specials, every slot free) can't, so the open layout's finals, scored with
// this layout's specials, bound it.
bool Search::CutByOpen(const PreparedLayout& layout, const std::array<std::vector<std::size_t>, kSlots>& specials,
                       double best) const {
  const bool open = std::ranges::all_of(specials, [](const auto& list) { return list.empty(); });
  if (open || open_finals_ == nullptr) return false;
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    for (const std::size_t sp : specials[slot]) {
      const StatEffect& e = model_.effect(options_[slot].specials[sp]);
      if (!e.conversions.empty() || e.tenths != Tenths{}) return false;
    }
  }
  std::vector<const Final*> group;
  for (const Final& f : *open_finals_) group.push_back(&f);
  return !AnyBeats(layout, std::move(group), best);
}

// Scores the layout's finals, keeping any build above the best so far.
void Search::ScoreLayout(const PreparedLayout& layout, const std::array<std::vector<std::size_t>, kSlots>& specials) {
  const bool open = std::ranges::all_of(specials, [](const auto& list) { return list.empty(); });
  Profile& profile = ProfileFor(specials);
  if (!profile.found) {
    profile.layout = &layout;
    FindFinals(profile);
  }
  if (open) open_finals_ = &profile.finals;
  std::vector<const Final*> all;
  for (const Final& f : profile.finals) all.push_back(&f);
  std::function<void(std::vector<const Final*>&)> search = [&](std::vector<const Final*>& group) {
    if (group.empty() || !CanBeat(layout, group, best_)) return;
    if (group.size() <= 4) {
      for (const Final* f : group) {
        const Build b = BuildOf(profile, specials, *f);
        const double score = scorer_.Score(layout, scorer_.Final(layout, StatsOf(model_, b)));
        ++counts_.scored;
        if (score > best_) {
          best_ = score;
          found_ = EnchantResult{score, b};
        }
      }
      return;
    }
    const std::size_t widest = Widest(group);
    std::ranges::stable_sort(group, {}, [&](const Final* f) { return f->stats[widest]; });
    const auto mid = static_cast<std::ptrdiff_t>(group.size() / 2);
    std::vector<const Final*> low(group.begin(), group.begin() + mid);
    std::vector<const Final*> high(group.begin() + mid, group.end());
    search(high);
    search(low);
  };
  search(all);
}

EnchantAssumptions Search::Assume() {
  const Dataset& d = model_.dataset();
  weapon_counts_ = scorer_.weights().weapon > 0;
  worn_ = model_.base().wearing;
  if (items_[kArmor] >= 0) worn_.push_back(d.armors[static_cast<std::size_t>(items_[kArmor])].type);
  if (items_[kRing] >= 0) worn_.push_back(d.rings[static_cast<std::size_t>(items_[kRing])].type);
  worn_.push_back(d.weapons[static_cast<std::size_t>(items_[kWeapon])].type);
  if (items_[kAbility] >= 0) worn_.push_back(d.abilities[static_cast<std::size_t>(items_[kAbility])].type);
  FindObtainable();
  read_ = {};
  FindReadStats();
  assumed_ = {};
  CompileOptions();
  assumed_.read = read_;
  return assumed_;
}

std::optional<EnchantResult> Search::Run(double incumbent) {
  const Dataset& d = model_.dataset();
  weapon_counts_ = scorer_.weights().weapon > 0;
  worn_ = model_.base().wearing;
  if (items_[kArmor] >= 0) worn_.push_back(d.armors[static_cast<std::size_t>(items_[kArmor])].type);
  if (items_[kRing] >= 0) worn_.push_back(d.rings[static_cast<std::size_t>(items_[kRing])].type);
  worn_.push_back(d.weapons[static_cast<std::size_t>(items_[kWeapon])].type);
  if (items_[kAbility] >= 0) worn_.push_back(d.abilities[static_cast<std::size_t>(items_[kAbility])].type);
  FindObtainable();
  read_ = {};
  // A stat the score turns out to read restarts the search with it.
  while (true) {
    boost_read_ = {};
    FindReadStats();
    assumed_ = {};
    CompileOptions();
    read_list_.clear();
    for (std::size_t s = 0; s < kStats; ++s) {
      if (read_[s]) read_list_.push_back(s);
    }
    if (read_list_.empty()) read_list_.push_back(0);
    profiles_.clear();
    flat_sets_.clear();
    found_.reset();
    best_ = incumbent;
    open_finals_ = nullptr;
    // Each item's clash-free sets of specials, the empty one first, so the
    // open layout comes first.
    std::array<std::vector<std::vector<std::size_t>>, kSlots> special_sets;
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
      const ItemOptions& item = options_[slot];
      if (!item.present) {
        special_sets[slot] = {{}};
        continue;
      }
      std::vector<std::size_t> ids(item.specials.size());
      for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = i;
      Sets(item, ids, item.room, [&](const Pick& p) { special_sets[slot].push_back(p.ids); });
    }
    // Layouts go in batches: prepared in order, then the read-stat guard and
    // the open layout's bound run on every thread, the bound against the
    // best found before the batch. Each is then scored in order, after a
    // second look at the bound if the best has risen since, so every layout
    // is cut exactly when it would be one at a time. The open layout goes
    // alone first, since the others are bounded by its finals.
    struct Pending {
      std::array<std::vector<std::size_t>, kSlots> specials;
      const PreparedLayout* layout = nullptr;
      int unread = -1;
      bool cut = false;
    };
    constexpr std::size_t kBatch = 64;
    std::size_t layouts = 1;
    for (const auto& sets : special_sets) layouts *= sets.size();
    std::size_t started = 0;
    bool restart = false;
    bool more = true;
    bool first = true;
    std::array<std::size_t, kSlots> pick{};
    std::vector<Pending> batch;
    while (more && !restart) {
      batch.clear();
      const std::size_t size = first ? 1 : kBatch;
      first = false;
      while (more && batch.size() < size) {
        Pending& p = batch.emplace_back();
        Layout key;
        key.items = items_;
        for (std::size_t slot = 0; slot < kSlots; ++slot) {
          p.specials[slot] = special_sets[slot][pick[slot]];
          for (const std::size_t sp : p.specials[slot]) key.specials[slot].push_back(options_[slot].specials[sp]);
          std::ranges::sort(key.specials[slot]);
        }
        p.layout = scorer_.Prepare(key);
        std::size_t slot = 0;
        for (; slot < kSlots; ++slot) {
          if (++pick[slot] < special_sets[slot].size()) break;
          pick[slot] = 0;
        }
        more = slot < kSlots;
        ++started;
      }
      const double before = best_;
      ParallelFor(batch.size(), [&](std::size_t i) {
        Pending& p = batch[i];
        if (p.layout == nullptr) return;
        p.unread = UnreadStat(*p.layout);
        if (p.unread < 0) p.cut = CutByOpen(*p.layout, p.specials, before);
      });
      std::size_t through = started - batch.size();
      // The share of layouts finished, as each next one starts.
      const auto report = [&] {
        if (settings_.progress) settings_.progress(static_cast<double>(through) / static_cast<double>(layouts));
        ++through;
      };
      for (const Pending& p : batch) {
        report();
        if (p.layout == nullptr) continue;
        if (p.unread >= 0) {
          read_[static_cast<std::size_t>(p.unread)] = true;
          restart = true;
          break;
        }
        ++counts_.layouts;
        if (p.cut || (best_ > before && CutByOpen(*p.layout, p.specials, best_))) {
          ++counts_.layouts_cut;
          continue;
        }
        ScoreLayout(*p.layout, p.specials);
      }
    }
    if (!restart) return found_;
  }
}

}  // namespace

std::optional<EnchantResult> BestEnchantments(const Model& model, Scorer& scorer,
                                              const std::array<std::int32_t, kSlots>& items,
                                              const EnchantSettings& settings, double incumbent,
                                              SearchCounts* counts) {
  SearchCounts ignored;
  Search search(model, scorer, items, settings, counts != nullptr ? *counts : ignored);
  return search.Run(incumbent);
}

EnchantAssumptions AssumptionsFor(const Model& model, Scorer& scorer, const std::array<std::int32_t, kSlots>& items,
                                  const EnchantSettings& settings) {
  SearchCounts counts;
  Search search(model, scorer, items, settings, counts);
  return search.Assume();
}

}  // namespace rotmg::search
