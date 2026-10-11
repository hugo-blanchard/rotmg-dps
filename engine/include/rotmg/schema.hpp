#pragma once

namespace rotmg {

/// Version of the dataset JSON this engine reads. Bump it whenever the
/// format changes in a way an older engine would misread.
inline constexpr int kSchemaVersion = 1;

/// Exact match only: datasets from older and newer pipelines are both rejected.
bool SupportsSchema(int version);

}  // namespace rotmg
