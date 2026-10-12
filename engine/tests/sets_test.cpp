#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "rotmg/sets.hpp"

namespace {

// Two cores that share a weapon (1) and an armor (2), like the Alien ones,
// each the key piece of its own set.
rotmg::Dataset Cores() {
  rotmg::Dataset d;
  for (const int core : {10, 11}) {
    rotmg::EquipmentSet& s = d.sets.emplace_back();
    s.type = core;
    s.pieces = {{3, core, ""}, {0, 1, ""}, {2, 2, ""}};
    s.key_pieces = {core};
    s.bonuses = {{2, {{"ATT", 5}}}, {3, {{"ATT", 7}}}};
  }
  return d;
}

}  // namespace

TEST_CASE("a set with key pieces only counts while one is worn") {
  const rotmg::Dataset d = Cores();
  CHECK(rotmg::WornSets(d, {1, 2}).empty());
  const std::vector<rotmg::WornSet> worn = rotmg::WornSets(d, {1, 2, 11});
  REQUIRE(worn.size() == 1);
  CHECK(worn[0].set->type == 11);
  CHECK(worn[0].pieces == 3);
  CHECK(worn[0].stats.size() == 2);
}

TEST_CASE("a set without key pieces counts any piece worn") {
  rotmg::Dataset d = Cores();
  d.sets[0].key_pieces.clear();
  const std::vector<rotmg::WornSet> worn = rotmg::WornSets(d, {1, 2});
  REQUIRE(worn.size() == 1);
  CHECK(worn[0].set->type == 10);
  CHECK(worn[0].pieces == 2);
}
