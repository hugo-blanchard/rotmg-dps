#include "rotmg/stats.hpp"

#include <algorithm>

namespace rotmg {

namespace {

constexpr std::array<int Character::*, kStats> kFields = {
    &Character::attack,   &Character::dexterity, &Character::speed,          &Character::wisdom,
    &Character::vitality, &Character::defense,   &Character::max_hit_points, &Character::max_magic_points};

}  // namespace

int StatIndex(std::string_view client_name) {
  const auto it = std::ranges::find(kStatNames, client_name);
  return it == kStatNames.end() ? -1 : static_cast<int>(it - kStatNames.begin());
}

Stats ToStats(const Character& character) {
  Stats out{};
  for (std::size_t s = 0; s < kStats; ++s) {
    out[s] = character.*kFields[s];
  }
  return out;
}

Character WithStats(Character character, const Stats& stats) {
  for (std::size_t s = 0; s < kStats; ++s) {
    character.*kFields[s] = stats[s];
  }
  return character;
}

Stats ToStats(const std::vector<StatBonus>& bonuses) {
  Stats out{};
  for (const StatBonus& b : bonuses) {
    if (const int s = StatIndex(b.stat); s >= 0) {
      out[static_cast<std::size_t>(s)] += b.amount;
    }
  }
  return out;
}

std::vector<StatBonus> BonusesOf(const Stats& stats) {
  std::vector<StatBonus> out;
  for (std::size_t s = 0; s < kStats; ++s) {
    if (stats[s] != 0) {
      out.push_back({kStatNames[s], stats[s]});
    }
  }
  return out;
}

bool AddStatMutator(const Mutator& m, EnchantStats& to) {
  if (m.tag != "ActivateOnEquip" || (m.text != "IncrementStat" && m.text != "BonusStatRelative")) {
    return false;
  }
  const int stat = StatIndex(m.attributes.Text("stat"));
  const double amount = m.attributes.Number("amount").value_or(0);
  if (m.text == "IncrementStat") {
    if (stat >= 0) {
      to.tenths[static_cast<std::size_t>(stat)] += static_cast<std::int32_t>(std::lround(amount * 10));
    }
  } else if (const int from = StatIndex(m.attributes.Text("statRelativeTo")); stat >= 0 && from >= 0) {
    to.conversions.push_back({static_cast<std::uint8_t>(from), static_cast<std::uint8_t>(stat), amount, false});
  }
  return true;
}

bool AddOwnConversion(const Activate& a, std::vector<Conversion>& to) {
  const bool of_total = a.type == "IncrementStatRelative";
  if (!of_total && a.type != "BonusStatRelative") {
    return false;
  }
  const int stat = StatIndex(a.Text("stat"));
  const int from = StatIndex(a.Text("statRelativeTo"));
  if (stat >= 0 && from >= 0) {
    to.push_back({static_cast<std::uint8_t>(from), static_cast<std::uint8_t>(stat), a.Number("amount").value_or(0),
                  of_total});
  }
  return true;
}

}  // namespace rotmg
