#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

#include "search/frontier.hpp"

namespace {

using rotmg::search::Point;

template <std::size_t D>
std::vector<Point<D>> RandomPoints(std::mt19937& rng, std::size_t n, std::int32_t range) {
  std::vector<Point<D>> out(n);
  for (Point<D>& p : out) {
    for (std::int32_t& v : p) v = static_cast<std::int32_t>(rng() % static_cast<unsigned>(range)) - range / 3;
  }
  return out;
}

// The frontier's values the slow way: points no other point covers, once
// each.
template <std::size_t D>
std::set<Point<D>> SlowFrontier(const std::vector<Point<D>>& points) {
  std::set<Point<D>> out;
  for (const Point<D>& p : points) {
    const bool beaten = std::ranges::any_of(points, [&](const Point<D>& q) {
      return q != p && rotmg::search::Covers(q, p);
    });
    if (!beaten) out.insert(p);
  }
  return out;
}

template <std::size_t D>
void CheckFrontier(std::mt19937& rng) {
  for (int trial = 0; trial < 300; ++trial) {
    const auto points = RandomPoints<D>(rng, 1 + rng() % 200, 2 + static_cast<std::int32_t>(rng() % 30));
    const std::vector<std::size_t> kept = rotmg::search::Frontier(points);
    std::set<Point<D>> values;
    for (const std::size_t i : kept) values.insert(points[i]);
    CHECK(values == SlowFrontier(points));
    CHECK(values.size() == kept.size());
    CHECK(std::ranges::is_sorted(kept));
  }
}

template <std::size_t D>
void CheckGrowingIndex(std::mt19937& rng) {
  for (int trial = 0; trial < 40; ++trial) {
    rotmg::search::GrowingIndex<D> index;
    std::vector<Point<D>> added;
    for (const Point<D>& p : RandomPoints<D>(rng, 400, 3 + static_cast<std::int32_t>(rng() % 40))) {
      if (rng() % 3 == 0) {
        index.Add(p);
        added.push_back(p);
        continue;
      }
      const bool covered = std::ranges::any_of(added, [&](const Point<D>& q) { return rotmg::search::Covers(q, p); });
      CHECK(index.Covers(p) == covered);
    }
  }
}

}  // namespace

TEST_CASE("a frontier keeps exactly the points nothing else covers") {
  std::mt19937 rng(3);
  CheckFrontier<1>(rng);
  CheckFrontier<2>(rng);
  CheckFrontier<3>(rng);
  CheckFrontier<4>(rng);
  CheckFrontier<5>(rng);
  CheckFrontier<6>(rng);
}

TEST_CASE("a growing index knows whether any point added covers another") {
  std::mt19937 rng(4);
  CheckGrowingIndex<1>(rng);
  CheckGrowingIndex<2>(rng);
  CheckGrowingIndex<3>(rng);
  CheckGrowingIndex<4>(rng);
  CheckGrowingIndex<5>(rng);
}
