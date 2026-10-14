#pragma once

// Pareto frontiers of integer points: the points no other point matches or
// beats on every dimension. Sizes are compile-time constants so a point is
// exactly as wide as it needs to be.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <type_traits>
#include <vector>

namespace rotmg::search {

template <std::size_t D>
using Point = std::array<std::int32_t, D>;

template <std::size_t D>
bool Covers(const Point<D>& a, const Point<D>& b) {
  for (std::size_t d = 0; d < D; ++d) {
    if (a[d] < b[d]) {
      return false;
    }
  }
  return true;
}

// Points on (a, b) of which none matches or beats another, a ascending and
// so b descending.
class Staircase {
 public:
  // Whether some point has at least a and at least b.
  bool Covers(std::int32_t a, std::int32_t b) const {
    const auto it = steps_.lower_bound(a);
    return it != steps_.end() && it->second >= b;
  }
  // Adds (a, b) and drops what it covers; does nothing if it's covered.
  void Add(std::int32_t a, std::int32_t b) {
    auto it = steps_.lower_bound(a);
    if (it != steps_.end() && it->second >= b) {
      return;
    }
    while (it != steps_.begin()) {
      const auto before = std::prev(it);
      if (before->second > b) {
        break;
      }
      steps_.erase(before);
    }
    if (it != steps_.end() && it->first == a) {
      steps_.erase(it);
    }
    steps_.emplace(a, b);
  }

 private:
  std::map<std::int32_t, std::int32_t> steps_;
};

// Answers "does some point cover x?" over a fixed set of points: for one
// dimension the maximum, for two a staircase, for more a Fenwick tree over
// the ranks of the first dimension (largest first) whose nodes index the
// rest. A query visits O(log^(D-2) n) staircases.
template <std::size_t D, std::size_t First = 0>
class StaticIndex {
 public:
  explicit StaticIndex(const std::vector<const Point<D>*>& points) {
    if constexpr (D - First == 1) {
      for (const Point<D>* p : points) most_ = std::max(most_, (*p)[First]);
    } else if constexpr (D - First == 2) {
      for (const Point<D>* p : points) stairs_.Add((*p)[First], (*p)[First + 1]);
    } else {
      for (const Point<D>* p : points) values_.push_back((*p)[First]);
      std::ranges::sort(values_, std::greater<>{});
      values_.erase(std::unique(values_.begin(), values_.end()), values_.end());
      std::vector<std::vector<const Point<D>*>> members(values_.size() + 1);
      for (const Point<D>* p : points) {
        for (std::size_t k = AtLeast((*p)[First]); k < members.size(); k += k & (~k + 1)) {
          members[k].push_back(p);
        }
      }
      nodes_.resize(members.size());
      for (std::size_t k = 1; k < members.size(); ++k) {
        if (!members[k].empty()) {
          nodes_[k] = std::make_unique<StaticIndex<D, First + 1>>(members[k]);
        }
      }
    }
  }

  bool Covers(const Point<D>& x) const {
    if constexpr (D - First == 1) {
      return most_ >= x[First];
    } else if constexpr (D - First == 2) {
      return stairs_.Covers(x[First], x[First + 1]);
    } else {
      for (std::size_t k = AtLeast(x[First]); k > 0; k -= k & (~k + 1)) {
        if (nodes_[k] && nodes_[k]->Covers(x)) {
          return true;
        }
      }
      return false;
    }
  }

 private:
  // How many distinct values are at least v: the tree's prefix to query.
  std::size_t AtLeast(std::int32_t v) const {
    return static_cast<std::size_t>(std::upper_bound(values_.begin(), values_.end(), v, std::greater<>{}) -
                                    values_.begin());
  }

  // Only an index on three or more dimensions has nodes.
  struct NoNodes {};
  using Nodes = std::conditional_t<(D - First > 2), std::vector<std::unique_ptr<StaticIndex<D, First + 1>>>, NoNodes>;

  std::int32_t most_ = std::numeric_limits<std::int32_t>::min();
  Staircase stairs_;
  std::vector<std::int32_t> values_;
  Nodes nodes_;
};

// The same question over a set that grows: static indexes of 16, 32, 64,
// ... points, merged and rebuilt like binary carries (the logarithmic
// method), and a buffer of the newest points searched one by one.
template <std::size_t D>
class GrowingIndex {
 public:
  void Add(const Point<D>& p) {
    buffer_.push_back(p);
    if (buffer_.size() < kBuffer) {
      return;
    }
    std::vector<Point<D>> carry = std::move(buffer_);
    buffer_.clear();
    std::size_t level = 0;
    for (; level < levels_.size() && !levels_[level].points.empty(); ++level) {
      carry.insert(carry.end(), levels_[level].points.begin(), levels_[level].points.end());
      levels_[level] = {};
    }
    if (level == levels_.size()) {
      levels_.emplace_back();
    }
    Level& l = levels_[level];
    l.points = std::move(carry);
    std::vector<const Point<D>*> pointers;
    pointers.reserve(l.points.size());
    for (const Point<D>& q : l.points) pointers.push_back(&q);
    l.index = std::make_unique<StaticIndex<D>>(pointers);
  }

  bool Covers(const Point<D>& x) const {
    for (const Point<D>& q : buffer_) {
      if (search::Covers(q, x)) {
        return true;
      }
    }
    for (const Level& l : levels_) {
      if (l.index && l.index->Covers(x)) {
        return true;
      }
    }
    return false;
  }

 private:
  static constexpr std::size_t kBuffer = 16;
  struct Level {
    std::vector<Point<D>> points;
    std::unique_ptr<StaticIndex<D>> index;
  };
  std::vector<Point<D>> buffer_;
  std::vector<Level> levels_;
};

// The indices of the frontier of `points`, ascending: one of each group of
// equal points. Taken in descending lexicographic order, anything that
// covers a point comes before it, so a point is kept unless a kept one
// covers it on the dimensions after the first: for 2 dimensions that's the
// most of the second so far, for 3 a staircase, for more a growing index.
template <std::size_t D>
std::vector<std::size_t> Frontier(const std::vector<Point<D>>& points) {
  std::vector<std::size_t> order(points.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::ranges::sort(order, [&](std::size_t x, std::size_t y) {
    return points[x] != points[y] ? points[x] > points[y] : x < y;
  });
  std::vector<std::size_t> kept;
  if constexpr (D == 1) {
    if (!order.empty()) kept.push_back(order[0]);
  } else if constexpr (D == 2) {
    std::int32_t most = std::numeric_limits<std::int32_t>::min();
    for (const std::size_t i : order) {
      if (kept.empty() || points[i][1] > most) {
        kept.push_back(i);
        most = points[i][1];
      }
    }
  } else if constexpr (D == 3) {
    Staircase stairs;
    for (const std::size_t i : order) {
      if (!stairs.Covers(points[i][1], points[i][2])) {
        kept.push_back(i);
        stairs.Add(points[i][1], points[i][2]);
      }
    }
  } else {
    GrowingIndex<D - 1> rest;
    for (const std::size_t i : order) {
      Point<D - 1> tail;
      std::copy(points[i].begin() + 1, points[i].end(), tail.begin());
      if (!rest.Covers(tail)) {
        kept.push_back(i);
        rest.Add(tail);
      }
    }
  }
  std::ranges::sort(kept);
  return kept;
}

}  // namespace rotmg::search
