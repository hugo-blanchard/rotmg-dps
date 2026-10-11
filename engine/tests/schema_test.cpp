#include <catch2/catch_test_macros.hpp>

#include "rotmg/schema.hpp"

TEST_CASE("accepts the current schema version") {
  CHECK(rotmg::SupportsSchema(rotmg::kSchemaVersion));
}

TEST_CASE("rejects older and newer schema versions") {
  CHECK_FALSE(rotmg::SupportsSchema(rotmg::kSchemaVersion - 1));
  CHECK_FALSE(rotmg::SupportsSchema(rotmg::kSchemaVersion + 1));
}
