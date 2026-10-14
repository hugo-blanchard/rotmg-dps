#pragma once

// A character's eight stats as arrays, and how gear, enchantments and
// relative bonuses add up to the stats a build is evaluated with. Requests
// and the exact search both work stats out here, so they agree to the
// point.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "rotmg/evaluate.hpp"
#include "rotmg/model.hpp"

namespace rotmg {

inline constexpr std::size_t kStats = 8;
/// Client names, in the order of every Stats array.
inline constexpr std::array<const char*, kStats> kStatNames = {"ATT", "DEX", "SPD", "WIS",
                                                               "VIT", "DEF", "MAXHP", "MAXMP"};
using Stats = std::array<int, kStats>;
/// Flat amounts in tenths of a point. The data's amounts are whole tenths
/// (+1.8 VIT), which the dataset checks, so sums of them are exact.
using Tenths = std::array<std::int32_t, kStats>;

/// A stat's index in kStatNames, or -1.
int StatIndex(std::string_view client_name);

/// "+16% ATT of Bonus ATT": `percent` of `from` above the class max goes to
/// `to`. With `of_total`, a share of the whole stat instead, like Brewer's
/// Bangle's "+15% VIT Equal to ATT".
struct Conversion {
  std::uint8_t from = 0;
  std::uint8_t to = 0;
  double percent = 0;
  bool of_total = false;
};

/// What enchantments add to stats: flat amounts summed, and their
/// conversions.
struct EnchantStats {
  Tenths tenths{};
  std::vector<Conversion> conversions;
};

/// Everything besides enchantments that decides a build's stats.
struct StatBase {
  /// Base stats, gear and sets.
  Stats before{};
  /// The weapon's and ability's own stat bonuses. The evaluation adds them
  /// later, but conversions count them.
  Stats item_bonuses{};
  /// The items' own conversions.
  std::vector<Conversion> own;
  /// Without a class, only shares of whole stats convert.
  Stats class_max{};
  bool has_class = false;
};

/// What each step of FinalStats added.
struct StatParts {
  Stats flat{};
  Stats converted{};
  Stats own{};
};

namespace detail {

inline Stats Converted(std::span<const Conversion> conversions, const Stats& everything, const StatBase& base) {
  std::array<double, kStats> sum{};
  for (const Conversion& c : conversions) {
    if (!c.of_total && !base.has_class) {
      continue;
    }
    const int of = c.of_total ? everything[c.from] : std::max(0, everything[c.from] - base.class_max[c.from]);
    sum[c.to] += c.percent / 100 * of;
  }
  Stats out{};
  for (std::size_t s = 0; s < kStats; ++s) {
    out[s] = static_cast<int>(std::floor(sum[s] + 1e-9));
  }
  return out;
}

}  // namespace detail

/// The stats a build's enchantments make on `base`. Flat amounts add up
/// over all items and each stat's total rounds down; that's an assumption.
/// RealmEye, Item Stat-Scaling: conversions all come from the stats before
/// any of them, so they don't feed each other. Bonus stats are those above
/// the class max, as on a maxed character. Each stat's conversions round
/// down, the enchantments' apart from the items' own.
inline Stats FinalStats(const StatBase& base, const Tenths& tenths, std::span<const Conversion> conversions,
                        StatParts* parts = nullptr) {
  Stats out = base.before;
  Stats flat{};
  for (std::size_t s = 0; s < kStats; ++s) {
    const std::int32_t t = tenths[s];
    // Rounded down, negative sums too.
    flat[s] = t >= 0 ? t / 10 : -((-t + 9) / 10);
    out[s] += flat[s];
  }
  if (parts != nullptr) {
    *parts = {flat, {}, {}};
  }
  if (conversions.empty() && base.own.empty()) {
    return out;
  }
  Stats everything{};
  for (std::size_t s = 0; s < kStats; ++s) {
    everything[s] = out[s] + base.item_bonuses[s];
  }
  const Stats converted = detail::Converted(conversions, everything, base);
  const Stats own = detail::Converted(base.own, everything, base);
  for (std::size_t s = 0; s < kStats; ++s) {
    out[s] += converted[s] + own[s];
  }
  if (parts != nullptr) {
    parts->converted = converted;
    parts->own = own;
  }
  return out;
}

/// The character's stats as an array.
Stats ToStats(const Character& character);
/// The character with these stats.
Character WithStats(Character character, const Stats& stats);
/// The bonuses as an array; stats outside the eight are left out.
Stats ToStats(const std::vector<StatBonus>& bonuses);
/// The nonzero stats as bonuses, in kStatNames order.
std::vector<StatBonus> BonusesOf(const Stats& stats);

/// Adds what an enchantment's mutator does to stats, if it does only that.
/// Precondition: the dataset was checked, so flat amounts are whole tenths.
bool AddStatMutator(const Mutator& mutator, EnchantStats& to);
/// An item's own on-equip conversion, if `effect` is one.
bool AddOwnConversion(const Activate& effect, std::vector<Conversion>& to);

}  // namespace rotmg
