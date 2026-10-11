#include "rotmg/schema.hpp"

namespace rotmg {

bool SupportsSchema(int version) { return version == kSchemaVersion; }

}  // namespace rotmg
