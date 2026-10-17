#pragma once

#include <functional>
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

  /// The DPS of the request's build with one slot changed in each of
  /// several ways, for choosing what goes in it. Shapes are in api.cpp.
  std::string EvaluateChoices(std::string_view request_json) const;

  /// Starts a simulation for the same kind of request as Evaluate, replacing
  /// any running one. The target sits at the hit model's distance. Returns an
  /// empty string on success, otherwise the error.
  std::string StartSimulation(std::string_view request_json);

  /// Moves the running simulation forward and returns its state as JSON.
  std::string AdvanceSimulation(double ms);

  /// Runs a simulation request many times with random rolls, each from its
  /// own seed, and returns how DPS and time to kill spread. Leaves any
  /// running simulation alone.
  std::string SimulateMany(std::string_view request_json) const;

  /// Looks for the build with the most calculated DPS: the items in the
  /// slots the request doesn't lock, and their enchantments if asked.
  /// `progress`, if set, gets a report every quarter second or so while it
  /// searches. Shapes are in api.cpp.
  std::string Optimize(std::string_view request_json,
                       const std::function<void(const std::string&)>& progress = {}) const;

  /// Roughly how long Optimize would take on the same request here, from
  /// timing a fifth of a second of evaluations. Shapes are in api.cpp.
  std::string EstimateOptimize(std::string_view request_json) const;

  /// What each item, enchantment and stat point adds to the request's own
  /// build, scored as Optimize would. Shapes are in api.cpp.
  std::string ExplainBuild(std::string_view request_json) const;

  /// The stats some damage source a class can use scales with, which are
  /// the ones Optimize can build around. Shapes are in api.cpp.
  std::string DamageStats(std::string_view request_json) const;

 private:
  std::optional<Dataset> dataset_;
  std::unique_ptr<Simulation> simulation_;
  // A second build in the same scene, to compare with.
  std::unique_ptr<Simulation> comparison_;
  // History buckets already sent, so each state carries only new ones.
  std::size_t history_sent_ = 0;
  std::size_t comparison_history_sent_ = 0;
};

}  // namespace rotmg
