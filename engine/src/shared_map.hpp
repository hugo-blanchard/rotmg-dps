#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rotmg {

// Hashes a value, or a pair or tuple of values, element by element.
template <typename T>
std::size_t HashOf(const T& value) {
  if constexpr (requires { std::hash<T>{}(value); }) {
    return std::hash<T>{}(value);
  } else {
    return std::apply(
        [](const auto&... parts) {
          std::size_t h = 0;
          ((h = h * 1000003 ^ HashOf(parts)), ...);
          return h;
        },
        value);
  }
}

// A map the search's threads share, split into parts with a lock each, so
// threads that look up different keys don't wait for each other: one lock
// for everything had them waiting for a third of the time. Nothing is ever
// removed, so a value stays where it is once added and a pointer or
// reference to it stays good.
template <typename Key, typename Value>
class SharedMap {
 public:
  // The value at `key`, or null.
  const Value* Find(const Key& key) const {
    const Part& part = PartOf(key);
    std::lock_guard lock(part.mu);
    const auto it = part.map.find(key);
    return it == part.map.end() ? nullptr : &it->second;
  }

  // Adds `value` at `key` unless there's one already, and returns the one
  // kept.
  const Value& Add(const Key& key, Value value) {
    Part& part = PartOf(key);
    std::lock_guard lock(part.mu);
    return part.map.try_emplace(key, std::move(value)).first->second;
  }

  // Calls `change` on the value at `key`, or on null, under the key's lock,
  // for updates that read the value first; `change` returns the new value
  // or nothing to leave it.
  template <typename Change>
  void Update(const Key& key, Change change) {
    Part& part = PartOf(key);
    std::lock_guard lock(part.mu);
    const auto it = part.map.find(key);
    if (auto value = change(it == part.map.end() ? nullptr : &it->second)) {
      part.map.insert_or_assign(key, std::move(*value));
    }
  }

  std::size_t Size() const {
    std::size_t n = 0;
    for (const Part& part : parts_) {
      std::lock_guard lock(part.mu);
      n += part.map.size();
    }
    return n;
  }

  // Every entry, by key, so whatever reads them in order doesn't depend on
  // how they're split.
  std::vector<std::pair<Key, Value>> Sorted() const {
    std::vector<std::pair<Key, Value>> out;
    for (const Part& part : parts_) {
      std::lock_guard lock(part.mu);
      out.insert(out.end(), part.map.begin(), part.map.end());
    }
    std::ranges::sort(out, {}, &std::pair<Key, Value>::first);
    return out;
  }

 private:
  struct Hash {
    std::size_t operator()(const Key& key) const { return HashOf(key); }
  };
  struct Part {
    mutable std::mutex mu;
    std::unordered_map<Key, Value, Hash> map;
  };
  static constexpr std::size_t kParts = 64;

  // The part a key's in. The hash is mixed first: std::hash of a number is
  // often the number itself, which would put most keys in a few parts.
  static std::size_t Index(const Key& key) {
    std::uint32_t h = static_cast<std::uint32_t>(HashOf(key));
    h ^= h >> 16;
    h *= 0x45d9f3bU;
    h ^= h >> 16;
    return h % kParts;
  }
  const Part& PartOf(const Key& key) const { return parts_[Index(key)]; }
  Part& PartOf(const Key& key) { return parts_[Index(key)]; }

  std::array<Part, kParts> parts_;
};

}  // namespace rotmg
