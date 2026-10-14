#include "enchantments.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <string_view>
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
// Layouts that leave the same stat options open share these finals. Each
// layout's are scored with ceilings: the score at the most of every read
// stat of a group of finals bounds every final in the group.

namespace rotmg::search {
namespace {

using Tenths = std::array<std::int32_t, kStats>;
using Mask = std::array<bool, kStats>;

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

// Whether mutator `a` does at least what `x` does, in a way that can't
// lower the score: higher damage and fire rate multipliers, a lower MP cost,
// a stronger chance boost of the same stat (on-shoot boosts never lower
// DPS when stronger, property-tested), more of a flat stat unless more of
// it can lower a read stat (`down`); anything else only when it's the same.
bool AtLeast(const Mutator& a, const Mutator& x, const Mask& down) {
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
    return number(a, "amount", 0) >= number(x, "amount", 0) && number(a, "duration", 0) >= number(x, "duration", 0) &&
           number(a, "proc", 1) >= number(x, "proc", 1) && number(a, "cooldown", 0) <= number(x, "cooldown", 0) &&
           same_except({"amount", "duration", "proc", "cooldown"});
  }
  if (a.tag == "ActivateOnEquip" && a.text == "IncrementStat") {
    const int stat = StatIndex(a.attributes.Text("stat"));
    const bool lowers = stat >= 0 && down[static_cast<std::size_t>(stat)];
    const double more = number(a, "amount", 0) - number(x, "amount", 0);
    return (lowers ? more == 0 : more >= 0) && same_except({"amount"});
  }
  return a.number == x.number && same_except({});
}

// Whether special `a`'s effects match `x`'s one for one, each at least as
// strong.
bool StrongerEffects(const Enchantment& a, const Enchantment& x, const Mask& down) {
  if (a.mutators.size() != x.mutators.size()) return false;
  std::vector<bool> used(a.mutators.size(), false);
  for (const Mutator& mx : x.mutators) {
    bool matched = false;
    for (std::size_t i = 0; i < a.mutators.size() && !matched; ++i) {
      if (!used[i] && AtLeast(a.mutators[i], mx, down)) {
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

 private:
  struct Profile;
  struct Assignment;

  void FindObtainable();
  void FindReadStats();
  bool ReadStatsHold(const PreparedLayout& layout);
  void CompileOptions();
  const FlatSets& SetsOf(std::size_t slot, const std::vector<std::size_t>& open, int room);
  Profile& ProfileFor(const std::array<std::vector<std::size_t>, kSlots>& specials);
  void FindFinals(Profile& profile);
  void Directions(std::span<const Conversion> conversions, const PreparedLayout& layout, Mask& up,
                  Mask& down) const;
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
  // share of it). A stat that does both is there twice, and then a sum's
  // best completion isn't a bound, so early dropping is off.
  std::vector<std::pair<std::size_t, int>> dims;
  bool mixed = false;
};

void Search::FindReadStats() {
  const Dataset& d = model_.dataset();
  const Weapon& weapon = d.weapons[static_cast<std::size_t>(items_[kWeapon])];
  const Ability* ability = items_[kAbility] < 0 ? nullptr : &d.abilities[static_cast<std::size_t>(items_[kAbility])];
  const Gear* armor = items_[kArmor] < 0 ? nullptr : &d.armors[static_cast<std::size_t>(items_[kArmor])];
  const Gear* ring = items_[kRing] < 0 ? nullptr : &d.rings[static_cast<std::size_t>(items_[kRing])];
  const auto mark = [](Mask& m, const std::string& stat) {
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
    mark(read_, a.Text("scalingStat"));
    if (on_shoot) mark(boost_read_, a.Text("scalingStat"));
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

// Whether the layout's score, without the tie-break, moves with any stat
// the search doesn't track. Each is raised a little and a lot from the
// layout's own stats. Not a proof, a guard: it catches a formula that
// starts reading a new stat.
bool Search::ReadStatsHold(const PreparedLayout& layout) {
  const double base = scorer_.Weighted(layout, scorer_.WithStats(layout, layout.stats));
  for (std::size_t s = 0; s < kStats; ++s) {
    if (read_[s]) continue;
    for (const int more : {20, 200}) {
      Stats st = layout.stats;
      st[s] += more;
      const double v = scorer_.Weighted(layout, scorer_.WithStats(layout, st));
      if (std::abs(v - base) > 1e-9 * std::max(1.0, std::abs(base))) {
        read_[s] = true;
        return false;
      }
    }
  }
  return true;
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
      const int stat = StatIndex(m.attributes.Text("stat"));
      const double proc = m.attributes.Number("proc").value_or(1);
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
  if (const PreparedLayout* open = scorer_.Prepare(Layout{items_, {}})) {
    for (const Conversion& c : open->own) {
      if (!read_[c.to]) continue;
      useful_[c.from] = true;
      if (c.percent < 0) down_[c.from] = true;
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
      if (counts) stats.push_back(std::move(o));
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
        const bool beaten = std::ranges::any_of(item.specials, [&](std::int32_t a) {
          if (a == x || !StrongerEffects(model_.enchantment(a), model_.enchantment(x), down_)) return false;
          const bool equal = StrongerEffects(model_.enchantment(x), model_.enchantment(a), down_);
          return (!equal || a < x) && covers_clashes(a, x);
        });
        if (!beaten) kept.push_back(x);
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
      bool beaten = false;
      for (std::size_t a = 0; a < stats.size() && !beaten; ++a) beaten = replaces(a, x);
      if (!beaten) keep.push_back(x);
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
  std::vector<Point<2 * kStats>> points(sets.size());
  for (std::size_t i = 0; i < sets.size(); ++i) {
    for (std::size_t k = 0; k < dims.size(); ++k) points[i][k] = dims[k].second * sets[i].tenths[dims[k].first];
  }
  auto kept = std::make_unique<FlatSets>();
  kept->most.fill(std::numeric_limits<std::int32_t>::min() / 4);
  kept->least.fill(std::numeric_limits<std::int32_t>::max() / 4);
  for (const std::size_t i : Frontier(points)) {
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
    if (read_[c.to]) up[c.from] = true;
  }
  for (const Conversion& c : layout.own) {
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
    // False when some stat moves read stats both ways: no best case then.
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
      Mask up{};
      Mask down{};
      Directions(conversions, layout, up, down);
      // Each stat at its best for the read stats: the most of it where more
      // raises them, the least where more lowers them.
      Tenths tenths = profile.fixed;
      bool bounded = true;
      for (std::size_t slot = 0; slot < kSlots; ++slot) {
        const Pick& pick = profile.conversions[slot][conv[slot]];
        for (std::size_t s = 0; s < kStats; ++s) {
          tenths[s] += pick.tenths[s];
          bounded = bounded && !(up[s] && down[s]);
          const FlatSets& sets = *profile.flats[slot][conv[slot]];
          tenths[s] += down[s] && !up[s] ? sets.least[s] : sets.most[s];
        }
      }
      Planned plan{conv, scorer_.FinalStats(layout, tenths, conversions), 0, bounded};
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
  std::vector<Point<kStats>> points(profile.finals.size());
  for (std::size_t i = 0; i < profile.finals.size(); ++i) {
    for (std::size_t k = 0; k < kStats; ++k) {
      points[i][k] = k < read_list_.size() ? profile.finals[i].stats[read_list_[k]] : 0;
    }
  }
  std::vector<Final> kept;
  for (const std::size_t i : Frontier(points)) kept.push_back(profile.finals[i]);
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
  struct Sum {
    Point<D> at{};
    std::array<std::uint16_t, kSlots> pick{};
  };
  // An item's flat set as a point: its tenths on each compared stat,
  // negated where more of the stat lowers the read stats.
  const auto point_of = [&](const FlatSet& f) {
    Point<D> p{};
    for (std::size_t k = 0; k < D; ++k) p[k] = a.dims[k].second * f.tenths[a.dims[k].first];
    return p;
  };
  // Each item's sets that nothing else of its own matches or beats on
  // these stats. They depend only on the item's sets and the stats, so
  // they're kept for other assignments.
  std::uint32_t signature = 0;
  for (const auto& [stat, sign] : a.dims) signature |= 1u << (stat + (sign < 0 ? kStats : 0));
  std::array<std::vector<std::pair<Point<D>, std::uint16_t>>, kSlots> own;
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    const std::vector<FlatSet>& sets = profile.flats[slot][a.conv[slot]]->sets;
    for (const std::uint16_t i : OwnFrontier<D>(sets, a, signature)) own[slot].emplace_back(point_of(sets[i]), i);
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
    if (a.mixed) return false;
    Point<D> best = at;
    for (std::size_t k = 0; k < D; ++k) best[k] += rest[slot][k];
    return reached.Covers(final_of(best));
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
  std::vector<Sum> sums = {Sum{}};
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    std::vector<Sum> grown;
    grown.reserve(sums.size() * own[slot].size());
    // Sums and the item's sets in blocks of nearby points. Pairs from two
    // blocks are all hopeless when the blocks' corners add to a hopeless
    // point, since the bound only grows; then each sum with the sets'
    // corner; then each pair.
    constexpr std::size_t kSumBlock = 16;
    constexpr std::size_t kSetBlock = 8;
    auto& sets = own[slot];
    std::ranges::sort(sets, std::greater<>{}, [](const auto& o) { return o.first; });
    std::ranges::sort(sums, std::greater<>{}, [](const Sum& x) { return x.at; });
    for (std::size_t ob = 0; ob < sets.size(); ob += kSetBlock) {
      const auto oe = std::min(sets.size(), ob + kSetBlock);
      const Point<D> sets_corner = corner(sets.begin() + static_cast<std::ptrdiff_t>(ob),
                                          sets.begin() + static_cast<std::ptrdiff_t>(oe),
                                          [](const auto& o) -> const Point<D>& { return o.first; });
      for (std::size_t sb = 0; sb < sums.size(); sb += kSumBlock) {
        const auto se = std::min(sums.size(), sb + kSumBlock);
        Point<D> both = corner(sums.begin() + static_cast<std::ptrdiff_t>(sb),
                               sums.begin() + static_cast<std::ptrdiff_t>(se),
                               [](const Sum& x) -> const Point<D>& { return x.at; });
        for (std::size_t k = 0; k < D; ++k) both[k] += sets_corner[k];
        if (hopeless(both, slot)) continue;
        for (std::size_t xi = sb; xi < se; ++xi) {
          const Sum& x = sums[xi];
          Point<D> with_corner = x.at;
          for (std::size_t k = 0; k < D; ++k) with_corner[k] += sets_corner[k];
          if (hopeless(with_corner, slot)) continue;
          for (std::size_t oi = ob; oi < oe; ++oi) {
            Sum next = x;
            for (std::size_t k = 0; k < D; ++k) next.at[k] += sets[oi].first[k];
            next.pick[slot] = sets[oi].second;
            if (!hopeless(next.at, slot)) grown.push_back(next);
          }
        }
      }
    }
    if (slot + 1 < kSlots) {
      std::vector<Point<D>> points;
      points.reserve(grown.size());
      for (const Sum& x : grown) points.push_back(x.at);
      std::vector<Sum> kept;
      for (const std::size_t i : Frontier(points)) kept.push_back(grown[i]);
      sums = std::move(kept);
      continue;
    }
    // Nothing comes after the last item: compare final stats.
    std::vector<Stats> finals;
    std::vector<Point<kStats>> points;
    finals.reserve(grown.size());
    for (const Sum& x : grown) {
      finals.push_back(final_of(x.at));
      Point<kStats> p{};
      for (std::size_t k = 0; k < read_list_.size(); ++k) p[k] = finals.back()[read_list_[k]];
      points.push_back(p);
    }
    for (const std::size_t i : Frontier(points)) {
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
  return out;
}

// Whether a build among the group's finals could score above `best`: the
// score at the most of every read stat over the group bounds them all.
bool Search::CanBeat(const PreparedLayout& layout, const std::vector<const Final*>& group, double best) const {
  Stats top = layout.stats;
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

// Scores the layout's finals, keeping any build above the best so far.
void Search::ScoreLayout(const PreparedLayout& layout, const std::array<std::vector<std::size_t>, kSlots>& specials) {
  const bool open = std::ranges::all_of(specials, [](const auto& list) { return list.empty(); });
  // A layout whose specials add no stats reaches no stats the open layout
  // (no specials, every slot free) can't, so the open layout's finals,
  // scored with this layout's specials, bound it.
  bool adds_stats = false;
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    for (const std::size_t sp : specials[slot]) {
      const StatEffect& e = model_.effect(options_[slot].specials[sp]);
      adds_stats = adds_stats || !e.conversions.empty() || e.tenths != Tenths{};
    }
  }
  if (!open && !adds_stats && open_finals_ != nullptr) {
    std::vector<const Final*> group;
    for (const Final& f : *open_finals_) group.push_back(&f);
    if (!AnyBeats(layout, std::move(group), best_)) {
      ++counts_.layouts_cut;
      return;
    }
  }
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
    bool restart = false;
    std::array<std::size_t, kSlots> pick{};
    while (true) {
      std::array<std::vector<std::size_t>, kSlots> specials;
      Layout key;
      key.items = items_;
      for (std::size_t slot = 0; slot < kSlots; ++slot) {
        specials[slot] = special_sets[slot][pick[slot]];
        for (const std::size_t sp : specials[slot]) key.specials[slot].push_back(options_[slot].specials[sp]);
        std::ranges::sort(key.specials[slot]);
      }
      if (const PreparedLayout* layout = scorer_.Prepare(key)) {
        if (!ReadStatsHold(*layout)) {
          restart = true;
          break;
        }
        ++counts_.layouts;
        ScoreLayout(*layout, specials);
      }
      std::size_t slot = 0;
      for (; slot < kSlots; ++slot) {
        if (++pick[slot] < special_sets[slot].size()) break;
        pick[slot] = 0;
      }
      if (slot == kSlots) break;
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

}  // namespace rotmg::search
