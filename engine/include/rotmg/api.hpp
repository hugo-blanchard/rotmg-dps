#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "rotmg/model.hpp"

namespace rotmg {

/// JSON in, JSON out, for callers that can't share C++ types, like the web
/// app through WebAssembly. Request and response shapes are in api.cpp.
class Api {
 public:
  /// Returns an empty string on success, otherwise the error.
  std::string Load(std::string_view dataset_json);

  /// Never throws: errors come back as {"ok": false, "error": "..."}.
  std::string Evaluate(std::string_view request_json) const;

 private:
  std::optional<Dataset> dataset_;
};

}  // namespace rotmg
