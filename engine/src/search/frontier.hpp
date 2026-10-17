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
#include <utility>
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

// "Does some point cover x?" over a fixed set, as fast as the dimension
// count allows: for 1 the maximum; for 2 the points sorted by the first
// dimension, largest first, with the running maximum of the second (one
// binary search); for 3 a table of the most of the third over the ranks of
// the first two (two binary searches and a lookup) while the table stays
// small; otherwise StaticIndex.
template <std::size_t D>
class SnapshotIndex {
 public:
  explicit SnapshotIndex(const std::vector<Point<D>>& points) {
    if constexpr (D == 1) {
      for (const Point<D>& p : points) most_ = std::max(most_, p[0]);
    } else if constexpr (D == 2) {
      std::vector<Point<D>> sorted = points;
      std::ranges::sort(sorted, std::greater<>{});
      for (const Point<D>& p : sorted) {
        firsts_.push_back(p[0]);
        most_second_.push_back(most_second_.empty() ? p[1] : std::max(most_second_.back(), p[1]));
      }
    } else if constexpr (D == 3) {
      for (const Point<D>& p : points) {
        firsts_.push_back(p[0]);
        seconds_.push_back(p[1]);
      }
      for (auto* values : {&firsts_, &seconds_}) {
        std::ranges::sort(*values, std::greater<>{});
        values->erase(std::unique(values->begin(), values->end()), values->end());
      }
      if (firsts_.size() * seconds_.size() > kMostCells) {
        UseStaticIndex(points);
        return;
      }
      // table_[i][j]: the most of the third dimension over points with at
      // least the i-th largest first value and the j-th largest second.
      const std::size_t n1 = seconds_.size();
      table_.assign(firsts_.size() * n1, std::numeric_limits<std::int32_t>::min());
      for (const Point<D>& p : points) {
        const std::size_t i = Rank(firsts_, p[0]) - 1;
        const std::size_t j = Rank(seconds_, p[1]) - 1;
        table_[i * n1 + j] = std::max(table_[i * n1 + j], p[2]);
      }
      for (std::size_t i = 0; i < firsts_.size(); ++i) {
        for (std::size_t j = 0; j < n1; ++j) {
          std::int32_t& cell = table_[i * n1 + j];
          if (i > 0) cell = std::max(cell, table_[(i - 1) * n1 + j]);
          if (j > 0) cell = std::max(cell, table_[i * n1 + j - 1]);
        }
      }
    } else {
      UseStaticIndex(points);
    }
  }

  bool Covers(const Point<D>& x) const {
    if (index_) {
      return index_->Covers(x);
    }
    if constexpr (D == 1) {
      return most_ >= x[0];
    } else if constexpr (D == 2) {
      const std::size_t n = Rank(firsts_, x[0]);
      return n > 0 && most_second_[n - 1] >= x[1];
    } else if constexpr (D == 3) {
      const std::size_t i = Rank(firsts_, x[0]);
      const std::size_t j = Rank(seconds_, x[1]);
      return i > 0 && j > 0 && table_[(i - 1) * seconds_.size() + j - 1] >= x[2];
    } else {
      return false;
    }
  }

 private:
  static constexpr std::size_t kMostCells = std::size_t{1} << 18;

  void UseStaticIndex(const std::vector<Point<D>>& points) {
    points_ = points;
    std::vector<const Point<D>*> pointers;
    pointers.reserve(points_.size());
    for (const Point<D>& p : points_) pointers.push_back(&p);
    index_ = std::make_unique<StaticIndex<D>>(pointers);
  }

  // How many of the values, largest first, are at least v.
  static std::size_t Rank(const std::vector<std::int32_t>& values, std::int32_t v) {
    return static_cast<std::size_t>(std::upper_bound(values.begin(), values.end(), v, std::greater<>{}) -
                                    values.begin());
  }

  std::int32_t most_ = std::numeric_limits<std::int32_t>::min();
  std::vector<std::int32_t> firsts_;
  std::vector<std::int32_t> seconds_;
  std::vector<std::int32_t> most_second_;
  std::vector<std::int32_t> table_;
  std::vector<Point<D>> points_;
  std::unique_ptr<StaticIndex<D>> index_;
};

// Buffers a thread reuses across frontiers, so finding one allocates only
// while they grow.
struct FrontierScratch {
  std::vector<std::uint32_t> order;
  // Three dimensions: the kept points' last two, as a staircase with the
  // first ascending and the second descending.
  std::vector<std::pair<std::int32_t, std::int32_t>> steps;
  // More: the kept points after their first dimension, and each block's most
  // of every dimension.
  std::vector<std::int32_t> tails;
  std::vector<std::int32_t> block_most;
};

// Past this many points, FrontierInto sorts kept points into buckets.
inline constexpr std::size_t kBucketedFrom = 512;
// Buckets are masks over the dimensions after the first.
inline constexpr std::size_t kMostBucketed = 11;

// FrontierInto's scan for many points: each kept point goes in a bucket by
// which dimensions after the first it has at least the pivot's value in,
// the pivot being the median of each. Only a point with at least the pivot
// wherever `p` has it can cover `p`, so only buckets whose mask holds p's
// are scanned.
template <std::size_t D, typename Get>
void BucketedScan(const std::vector<std::uint32_t>& order, const Get& point, std::vector<std::uint32_t>& kept) {
  constexpr std::size_t kTail = D - 1;
  constexpr std::size_t kBlock = 32;
  Point<D> pivot{};
  {
    std::vector<std::int32_t> values(order.size());
    for (std::size_t d = 1; d < D; ++d) {
      for (std::size_t i = 0; i < order.size(); ++i) values[i] = point(order[i])[d];
      const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
      std::nth_element(values.begin(), mid, values.end());
      pivot[d] = *mid;
    }
  }
  struct Bucket {
    std::vector<std::int32_t> tails;
    std::vector<std::int32_t> most;
  };
  std::vector<Bucket> buckets(std::size_t{1} << kTail);
  std::vector<std::uint32_t> used;
  for (const std::uint32_t i : order) {
    const Point<D>& p = point(i);
    std::uint32_t mask = 0;
    for (std::size_t d = 0; d < kTail; ++d) mask |= static_cast<std::uint32_t>(p[d + 1] >= pivot[d + 1]) << d;
    bool covered = false;
    for (std::size_t u = 0; u < used.size() && !covered; ++u) {
      if ((used[u] & mask) != mask) continue;
      const Bucket& bucket = buckets[used[u]];
      const std::size_t count = bucket.tails.size() / kTail;
      for (std::size_t b = 0; b * kBlock < count && !covered; ++b) {
        const std::int32_t* m = bucket.most.data() + b * kTail;
        bool may = true;
        for (std::size_t d = 0; d < kTail && may; ++d) may = m[d] >= p[d + 1];
        if (!may) continue;
        const std::size_t end = std::min(count, (b + 1) * kBlock);
        for (std::size_t k = b * kBlock; k < end && !covered; ++k) {
          const std::int32_t* q = bucket.tails.data() + k * kTail;
          bool all = true;
          for (std::size_t d = 0; d < kTail && all; ++d) all = q[d] >= p[d + 1];
          covered = all;
        }
      }
    }
    if (covered) continue;
    Bucket& bucket = buckets[mask];
    const std::size_t count = bucket.tails.size() / kTail;
    if (count == 0) used.push_back(mask);
    if (count % kBlock == 0) bucket.most.insert(bucket.most.end(), p.begin() + 1, p.end());
    std::int32_t* m = bucket.most.data() + (count / kBlock) * kTail;
    for (std::size_t d = 0; d < kTail; ++d) m[d] = std::max(m[d], p[d + 1]);
    bucket.tails.insert(bucket.tails.end(), p.begin() + 1, p.end());
    kept.push_back(i);
  }
}

// The indices of the frontier of `n` points, `point(i)` the i-th, ascending
// in `kept`: one of each group of equal points, the first. Taken in
// descending lexicographic order, anything that covers a point comes before
// it, so a point is kept unless a kept one covers it on the dimensions after
// the first: for 2 dimensions that's the most of the second so far, for 3 a
// staircase, for more a scan of the kept points in blocks, skipping a block
// whose maxima can't cover the point. Frontiers in the search are small
// (tens to hundreds of points), where the scan beats building an index.
template <std::size_t D, typename Get>
void FrontierInto(std::size_t n, const Get& point, FrontierScratch& scratch, std::vector<std::uint32_t>& kept) {
  kept.clear();
  std::vector<std::uint32_t>& order = scratch.order;
  order.resize(n);
  std::iota(order.begin(), order.end(), std::uint32_t{0});
  std::ranges::sort(order, [&](std::uint32_t x, std::uint32_t y) {
    const Point<D>& a = point(x);
    const Point<D>& b = point(y);
    return a != b ? a > b : x < y;
  });
  if constexpr (D == 1) {
    if (!order.empty()) kept.push_back(order[0]);
  } else if constexpr (D == 2) {
    std::int32_t most = std::numeric_limits<std::int32_t>::min();
    for (const std::uint32_t i : order) {
      if (kept.empty() || point(i)[1] > most) {
        kept.push_back(i);
        most = point(i)[1];
      }
    }
  } else if constexpr (D == 3) {
    auto& steps = scratch.steps;
    steps.clear();
    for (const std::uint32_t i : order) {
      const std::int32_t a = point(i)[1];
      const std::int32_t b = point(i)[2];
      auto it = std::ranges::lower_bound(steps, a, {}, &std::pair<std::int32_t, std::int32_t>::first);
      if (it != steps.end() && it->second >= b) continue;
      kept.push_back(i);
      // Steps before it have less of a; those with no more of b are covered.
      auto from = it;
      while (from != steps.begin() && std::prev(from)->second <= b) --from;
      it = steps.erase(from, it);
      if (it != steps.end() && it->first == a) it = steps.erase(it);
      steps.insert(it, {a, b});
    }
  } else if (n > kBucketedFrom && D - 1 <= kMostBucketed) {
    BucketedScan<D>(order, point, kept);
  } else {
    constexpr std::size_t kTail = D - 1;
    constexpr std::size_t kBlock = 32;
    auto& tails = scratch.tails;
    auto& most = scratch.block_most;
    tails.clear();
    most.clear();
    for (const std::uint32_t i : order) {
      const Point<D>& p = point(i);
      const std::size_t count = tails.size() / kTail;
      bool covered = false;
      for (std::size_t b = 0; b * kBlock < count && !covered; ++b) {
        const std::int32_t* m = most.data() + b * kTail;
        bool may = true;
        for (std::size_t d = 0; d < kTail && may; ++d) may = m[d] >= p[d + 1];
        if (!may) continue;
        const std::size_t end = std::min(count, (b + 1) * kBlock);
        for (std::size_t k = b * kBlock; k < end && !covered; ++k) {
          const std::int32_t* q = tails.data() + k * kTail;
          bool all = true;
          for (std::size_t d = 0; d < kTail && all; ++d) all = q[d] >= p[d + 1];
          covered = all;
        }
      }
      if (covered) continue;
      if (count % kBlock == 0) most.insert(most.end(), p.begin() + 1, p.end());
      std::int32_t* m = most.data() + (count / kBlock) * kTail;
      for (std::size_t d = 0; d < kTail; ++d) m[d] = std::max(m[d], p[d + 1]);
      tails.insert(tails.end(), p.begin() + 1, p.end());
      kept.push_back(i);
    }
  }
  std::ranges::sort(kept);
}

template <std::size_t D>
std::vector<std::size_t> Frontier(const std::vector<Point<D>>& points) {
  FrontierScratch scratch;
  std::vector<std::uint32_t> kept;
  FrontierInto<D>(points.size(), [&](std::uint32_t i) -> const Point<D>& { return points[i]; }, scratch, kept);
  return {kept.begin(), kept.end()};
}

// The same for points whose dimension count is only known at run time, as
// rows of `dims` values: for paths where instantiating a template for every
// count isn't worth it. Always the block scan.
inline void FrontierOfRowsInto(const std::vector<std::int32_t>& rows, std::size_t dims, FrontierScratch& scratch,
                               std::vector<std::uint32_t>& kept) {
  kept.clear();
  const std::size_t n = dims == 0 ? 0 : rows.size() / dims;
  const auto row = [&](std::size_t i) { return rows.data() + i * dims; };
  // Many rows are worth copying into points for FrontierInto's faster scans.
  if (n > kBucketedFrom && dims >= 2 && dims <= 12) {
    const auto as_points = [&]<std::size_t D>(std::integral_constant<std::size_t, D>) {
      std::vector<Point<D>> points(n);
      for (std::size_t i = 0; i < n; ++i) std::copy(row(i), row(i) + D, points[i].begin());
      FrontierInto<D>(n, [&](std::uint32_t i) -> const Point<D>& { return points[i]; }, scratch, kept);
    };
    [&]<std::size_t... D>(std::index_sequence<D...>) {
      ((dims == D + 2 ? as_points(std::integral_constant<std::size_t, D + 2>{}) : void()), ...);
    }(std::make_index_sequence<11>{});
    return;
  }
  std::vector<std::uint32_t>& order = scratch.order;
  order.resize(n);
  std::iota(order.begin(), order.end(), std::uint32_t{0});
  std::ranges::sort(order, [&](std::uint32_t x, std::uint32_t y) {
    const auto mismatch = std::mismatch(row(x), row(x) + dims, row(y));
    return mismatch.first != row(x) + dims ? *mismatch.first > *mismatch.second : x < y;
  });
  constexpr std::size_t kBlock = 32;
  auto& most = scratch.block_most;
  most.clear();
  for (const std::uint32_t i : order) {
    bool covered = false;
    for (std::size_t b = 0; b * kBlock < kept.size() && !covered; ++b) {
      bool may = true;
      for (std::size_t d = 0; d < dims && may; ++d) may = most[b * dims + d] >= row(i)[d];
      if (!may) continue;
      for (std::size_t k = b * kBlock; k < std::min(kept.size(), (b + 1) * kBlock) && !covered; ++k) {
        bool all = true;
        for (std::size_t d = 0; d < dims && all; ++d) all = row(kept[k])[d] >= row(i)[d];
        covered = all;
      }
    }
    if (covered) continue;
    if (kept.size() % kBlock == 0) most.insert(most.end(), row(i), row(i) + dims);
    const std::size_t b = kept.size() / kBlock;
    for (std::size_t d = 0; d < dims; ++d) most[b * dims + d] = std::max(most[b * dims + d], row(i)[d]);
    kept.push_back(i);
  }
  std::ranges::sort(kept);
}

inline std::vector<std::size_t> FrontierOfRows(const std::vector<std::int32_t>& rows, std::size_t dims) {
  FrontierScratch scratch;
  std::vector<std::uint32_t> kept;
  FrontierOfRowsInto(rows, dims, scratch, kept);
  return {kept.begin(), kept.end()};
}

}  // namespace rotmg::search
