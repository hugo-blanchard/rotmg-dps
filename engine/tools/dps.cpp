// rotmg-dps prints a weapon's DPS against a range of DEF values.

#include <charconv>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "rotmg/dataset.hpp"
#include "rotmg/evaluate.hpp"

namespace {

constexpr const char* kUsage = R"(usage: rotmg-dps <dataset.json> <weapon name> [options]

  --att N         ATT without the weapon (default 50)
  --dex N         DEX without the weapon (default 50)
  --status LIST   player effects, comma-separated: weak, damaging, berserk, dazed
  --target LIST   target effects, comma-separated: armored, armor-broken, exposed, cursed
  --def-max N     highest DEF in the table (default 100)
  --def-step N    DEF step (default 10)
)";

bool ParseInt(std::string_view text, int& out) {
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  return ec == std::errc() && end == text.data() + text.size();
}

std::vector<std::string_view> SplitCommas(std::string_view list) {
  std::vector<std::string_view> parts;
  while (!list.empty()) {
    const std::size_t comma = list.find(',');
    parts.push_back(list.substr(0, comma));
    list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
  }
  return parts;
}

std::string Join(const std::vector<std::string>& items) {
  std::string out;
  for (const std::string& item : items) {
    out += (out.empty() ? "" : "; ") + item;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << kUsage;
    return 2;
  }
  rotmg::Character character{.attack = 50, .dexterity = 50};
  rotmg::Target target;
  int def_max = 100;
  int def_step = 10;
  for (int i = 3; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (i + 1 >= argc) {
      std::cerr << "missing value for " << flag << "\n";
      return 2;
    }
    const std::string_view value = argv[++i];
    bool ok = true;
    if (flag == "--att") {
      ok = ParseInt(value, character.attack);
    } else if (flag == "--dex") {
      ok = ParseInt(value, character.dexterity);
    } else if (flag == "--def-max") {
      ok = ParseInt(value, def_max);
    } else if (flag == "--def-step") {
      ok = ParseInt(value, def_step) && def_step > 0;
    } else if (flag == "--status") {
      for (std::string_view s : SplitCommas(value)) {
        if (s == "weak") character.status.weak = true;
        else if (s == "damaging") character.status.damaging = true;
        else if (s == "berserk") character.status.berserk = true;
        else if (s == "dazed") character.status.dazed = true;
        else ok = false;
      }
    } else if (flag == "--target") {
      for (std::string_view s : SplitCommas(value)) {
        if (s == "armored") target.armored = true;
        else if (s == "armor-broken") target.armor_broken = true;
        else if (s == "exposed") target.exposed = true;
        else if (s == "cursed") target.cursed = true;
        else ok = false;
      }
    } else {
      std::cerr << "unknown option " << flag << "\n" << kUsage;
      return 2;
    }
    if (!ok) {
      std::cerr << "bad value for " << flag << ": " << value << "\n";
      return 2;
    }
  }

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::cerr << "can't open " << argv[1] << "\n";
    return 1;
  }
  std::ostringstream text;
  text << in.rdbuf();
  rotmg::Dataset dataset;
  try {
    dataset = rotmg::ParseDataset(text.str());
  } catch (const rotmg::DatasetError& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
  const rotmg::Weapon* weapon = rotmg::FindWeapon(dataset, argv[2]);
  if (weapon == nullptr) {
    std::cerr << "no weapon named \"" << argv[2] << "\" in " << argv[1] << "\n";
    return 1;
  }

  std::printf("%s, build %s\n", weapon->name.c_str(), dataset.build.c_str());
  std::printf("ATT %d, DEX %d before the weapon's bonuses\n\n", character.attack, character.dexterity);
  std::printf("%5s  %9s\n", "DEF", "DPS");
  rotmg::Breakdown last;
  for (int def = 0; def <= def_max; def += def_step) {
    target.defense = def;
    last = rotmg::EvaluateWeapon(*weapon, character, target);
    std::printf("%5d  %9.1f\n", def, last.dps);
  }
  std::printf("\nAssumptions: %s\n", Join(last.assumptions).c_str());
  if (!last.unsupported.empty()) {
    std::printf("Not modelled yet: %s\n", Join(last.unsupported).c_str());
  }
  return 0;
}
