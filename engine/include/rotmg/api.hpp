#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "rotmg/model.hpp"
#include "rotmg/sim.hpp"

namespace rotmg {

/// JSON in, JSON out, for callers that can't share C++ types, like the web
/// app through WebAssembly. Request and response shapes are in api.cpp.
class Api {
 public:
  /// Returns an empty string on success, otherwise the error.
  std::string Load(std::string_view dataset_json);

  /// Never throws: errors come back as {"ok": false, "error": "..."}.
  std::string Evaluate(std::string_view request_json) const;

  /// Starts a simulation for the same kind of request as Evaluate, replacing
  /// any running one. The target sits at the hit model's distance. Returns an
  /// empty string on success, otherwise the error.
  std::string StartSimulation(std::string_view request_json);

  /// Moves the running simulation forward and returns its state as JSON.
  std::string AdvanceSimulation(double ms);

 private:
  std::optional<Dataset> dataset_;
  std::unique_ptr<Simulation> simulation_;
  // History buckets already sent, so each state carries only new ones.
  std::size_t history_sent_ = 0;
};

}  // namespace rotmg
