#include "rotmg/optimize.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include "rotmg/dataset.hpp"
#include "rotmg/enchant.hpp"

namespace rotmg {
namespace {

constexpr std::size_t kSlotCount = 4;
constexpr std::array<const char*, kSlotCount> kSlotLabels = {"WEAPON", "ABILITY", "ARMOR", "RING"};

using Build = OptimizerBuild;
using Mode = OptimizerProblem::Enchantments;

class BuildSearch {
 public:
  BuildSearch(const Dataset& d, const OptimizerProblem& p) : d_(d), p_(p) {}

  std::optional<OptimizerResult> Run();

 private:
  // What can go on the item in a slot, by the game's rules, and how many.
  struct Fitting {
    std::vector<const Enchantment*> options;
    int slots = 0;
  };

  const Fitting& Fits(std::size_t slot, const std::string& item);
  void Adapt(Build& b, std::size_t slot);
  std::optional<double> Score(const Build& b);
  const std::vector<const Enchantment*>& Relevant(const Build& b, std::size_t slot);
  double Fill(Build& b, std::size_t slot);
  const Enchantment* Find(const std::string& id) const;

  const Dataset& d_;
  const OptimizerProblem& p_;
  std::map<std::pair<std::size_t, std::string>, Fitting> fitting_;
  std::map<std::string, std::optional<double>> scores_;
  std::map<std::string, std::pair<double, Build>> seen_;
  std::map<std::pair<std::size_t, std::string>, bool> generic_;
  std::map<std::pair<std::size_t, std::string>, std::vector<const Enchantment*>> relevant_;
  long evaluated_ = 0;
};

const Enchantment* BuildSearch::Find(const std::string& id) const {
  const auto it = std::ranges::find(d_.enchantments, id, &Enchantment::id);
  return it == d_.enchantments.end() ? nullptr : &*it;
}

const BuildSearch::Fitting& BuildSearch::Fits(std::size_t slot, const std::string& item) {
  const auto key = std::pair{slot, item};
  if (auto it = fitting_.find(key); it != fitting_.end()) {
    return it->second;
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
  return fitting_.emplace(key, std::move(out)).first->second;
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

std::optional<double> BuildSearch::Score(const Build& b) {
  std::string items_key;
  for (const std::string& item : b.items) {
    items_key += item + "\n";
  }
  std::string key = items_key;
  for (const auto& list : b.enchants) {
    for (const std::string& id : list) {
      key += id + ",";
    }
    key += "\n";
  }
  if (auto it = scores_.find(key); it != scores_.end()) {
    return it->second;
  }
  ++evaluated_;
  const std::optional<double> dps = p_.score(b);
  scores_.emplace(key, dps);
  // One entry per set of items, with its best enchantments.
  if (auto it = seen_.find(items_key); dps && (it == seen_.end() || it->second.first < *dps)) {
    seen_[items_key] = {*dps, b};
  }
  return dps;
}

// The enchantments worth trying on an item: ones that change the DPS on
// their own. Defense, HP and loot never do. One that any item of the slot
// takes (a generic stat bonus) is tested once per slot; one made for
// certain items, like an awakening, once per item.
const std::vector<const Enchantment*>& BuildSearch::Relevant(const Build& b, std::size_t slot) {
  const auto key = std::pair{slot, b.items[slot]};
  if (auto it = relevant_.find(key); it != relevant_.end()) {
    return it->second;
  }
  Build bare = b;
  bare.enchants[slot].clear();
  const double without = Score(bare).value_or(0);
  const auto changes = [&](const Enchantment* e) {
    bare.enchants[slot] = {e->id};
    const auto s = Score(bare);
    return s && std::abs(*s - without) > without * 1e-9;
  };
  std::vector<const Enchantment*> out;
  for (const Enchantment* e : Fits(slot, b.items[slot]).options) {
    if (!FitsSlot(*e, kSlotLabels[slot])) {
      if (changes(e)) out.push_back(e);
      continue;
    }
    const auto generic = std::pair{slot, e->id};
    auto it = generic_.find(generic);
    if (it == generic_.end()) {
      it = generic_.emplace(generic, changes(e)).first;
    }
    if (it->second) out.push_back(e);
  }
  return relevant_.emplace(key, std::move(out)).first->second;
}

// Fills the slot's item with the enchantments that add the most, one at a
// time, while one still adds something.
double BuildSearch::Fill(Build& b, std::size_t slot) {
  b.enchants[slot].clear();
  double current = Score(b).value_or(0);
  if (p_.enchantments != Mode::kBest || b.items[slot].empty()) {
    return current;
  }
  const auto& options = Relevant(b, slot);
  for (int k = 0; k < std::min(p_.per_item, Fits(slot, b.items[slot]).slots); ++k) {
    const Enchantment* best = nullptr;
    double best_dps = current;
    for (const Enchantment* e : options) {
      const bool taken = std::ranges::any_of(b.enchants[slot], [&](const std::string& id) {
        const Enchantment* other = Find(id);
        return other->id == e->id || Conflict(*other, *e);
      });
      if (taken) {
        continue;
      }
      b.enchants[slot].push_back(e->id);
      if (const auto s = Score(b); s && *s > best_dps * (1 + 1e-9)) {
        best_dps = *s;
        best = e;
      }
      b.enchants[slot].pop_back();
    }
    if (best == nullptr) {
      break;
    }
    b.enchants[slot].push_back(best->id);
    current = best_dps;
  }
  return current;
}

std::optional<OptimizerResult> BuildSearch::Run() {
  const auto& candidates = p_.candidates;
  const bool best = p_.enchantments == Mode::kBest;
  const auto baseline = Score(p_.start);
  if (!baseline) {
    return std::nullopt;
  }
  Build current = p_.start;
  double current_dps = *baseline;
  const auto fill_all = [&] {
    if (!best) {
      return;
    }
    for (std::size_t i = 0; i < kSlotCount; ++i) {
      current_dps = Fill(current, i);
    }
  };

  // Coordinate descent: the best item for one slot with the others as they
  // are, slot by slot, then whole sets at once so set bonuses count, until
  // nothing improves. Items are ranked with the enchantments they keep,
  // then the best few are filled; everything is filled again between
  // rounds.
  fill_all();
  for (int round = 0; round < 6; ++round) {
    bool improved = false;
    const auto take = [&](const Build& next, double dps) {
      if (dps > current_dps * (1 + 1e-9)) {
        current = next;
        current_dps = dps;
        improved = true;
      }
    };
    for (std::size_t slot = 0; slot < kSlotCount; ++slot) {
      std::vector<std::pair<double, Build>> tried;
      for (const std::string& name : candidates[slot]) {
        if (name == current.items[slot]) {
          continue;
        }
        Build next = current;
        next.items[slot] = name;
        Adapt(next, slot);
        if (const auto s = Score(next)) {
          tried.emplace_back(*s, std::move(next));
        }
      }
      std::ranges::sort(tried, [](const auto& a, const auto& b) { return a.first > b.first; });
      if (best) {
        // The best few items with their own enchantments filled in, since
        // some take enchantments others can't, like awakenings.
        for (std::size_t i = 0; i < tried.size() && i < 8; ++i) {
          tried[i].first = Fill(tried[i].second, slot);
        }
        std::ranges::sort(tried, [](const auto& a, const auto& b) { return a.first > b.first; });
      }
      if (!tried.empty()) {
        take(tried.front().second, tried.front().first);
      }
    }
    for (const EquipmentSet& set : d_.sets) {
      Build next = current;
      int changed = 0;
      for (const EquipmentSet::Piece& piece : set.pieces) {
        const auto slot = static_cast<std::size_t>(piece.slot);
        const bool usable =
            slot < kSlotCount && std::ranges::find(candidates[slot], piece.name) != candidates[slot].end();
        if (usable && next.items[slot] != piece.name) {
          next.items[slot] = piece.name;
          Adapt(next, slot);
          ++changed;
        }
      }
      if (const auto s = changed >= 2 ? Score(next) : std::nullopt) {
        take(next, *s);
      }
    }
    const double before = current_dps;
    fill_all();
    if (!improved && current_dps <= before * (1 + 1e-9)) {
      break;
    }
  }

  // Greedy filling can settle on a worse set; swapping one enchantment at a
  // time for another that fits fixes most of that.
  for (int pass = 0; best && pass < 3; ++pass) {
    bool swapped = false;
    for (std::size_t slot = 0; slot < kSlotCount; ++slot) {
      if (current.items[slot].empty()) {
        continue;
      }
      const auto& options = Relevant(current, slot);
      for (std::size_t k = 0; k < current.enchants[slot].size(); ++k) {
        for (const Enchantment* e : options) {
          Build next = current;
          next.enchants[slot][k] = e->id;
          const bool clashes = std::ranges::any_of(next.enchants[slot], [&](const std::string& id) {
            const Enchantment* other = Find(id);
            return other->id != e->id && Conflict(*other, *e);
          });
          const bool twice = std::ranges::count(next.enchants[slot], e->id) > 1;
          if (clashes || twice) {
            continue;
          }
          if (const auto s = Score(next); s && *s > current_dps * (1 + 1e-9)) {
            current = std::move(next);
            current_dps = *s;
            swapped = true;
          }
        }
      }
    }
    if (!swapped) {
      break;
    }
  }

  OptimizerResult out;
  out.baseline = *baseline;
  out.evaluated = evaluated_;
  for (auto& [key, entry] : seen_) {
    out.builds.push_back(entry);
  }
  std::ranges::sort(out.builds, [](const auto& a, const auto& b) { return a.first > b.first; });
  if (out.builds.size() > p_.results) {
    out.builds.resize(p_.results);
  }
  return out;
}

}  // namespace

std::optional<OptimizerResult> SearchBuilds(const Dataset& dataset, const OptimizerProblem& problem) {
  return BuildSearch(dataset, problem).Run();
}

}  // namespace rotmg
