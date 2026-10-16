#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "shared_map.hpp"

TEST_CASE("a shared map keeps the first value added at a key, from any thread") {
  rotmg::SharedMap<std::tuple<int, std::string>, int> map;
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&map, t] {
      for (int i = 0; i < 1000; ++i) {
        map.Add({i, "key"}, i * 10 + t);
      }
    });
  }
  for (std::thread& t : threads) t.join();
  CHECK(map.Size() == 1000);
  // Whichever thread got there first.
  const int* kept = map.Find({42, "key"});
  REQUIRE(kept != nullptr);
  CHECK(*kept / 10 == 42);
  CHECK(map.Find({42, "other"}) == nullptr);
  CHECK(&map.Add({42, "key"}, 7) == kept);
}

TEST_CASE("a shared map updates a value from what it holds and lists entries by key") {
  rotmg::SharedMap<std::string, int> best;
  for (const int score : {3, 9, 4}) {
    best.Update("build", [&](const int* now) -> std::optional<int> {
      if (now == nullptr || *now < score) return score;
      return std::nullopt;
    });
  }
  best.Add("another", 1);
  best.Add("a third", 2);
  CHECK(*best.Find("build") == 9);
  const auto sorted = best.Sorted();
  REQUIRE(sorted.size() == 3);
  CHECK(sorted[0].first == "a third");
  CHECK(sorted[1].first == "another");
  CHECK(sorted[2].first == "build");
}
