// Exposes rotmg::Api to JavaScript. The web app runs it in a Web Worker.

#include <string>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "rotmg/api.hpp"

EMSCRIPTEN_BINDINGS(rotmg) {
  using emscripten::optional_override;
  emscripten::class_<rotmg::Api>("Api")
      .constructor<>()
      .function("load", optional_override([](rotmg::Api& api, const std::string& json) { return api.Load(json); }))
      .function("evaluate",
                optional_override([](const rotmg::Api& api, const std::string& json) { return api.Evaluate(json); }))
      .function("evaluateChoices", optional_override([](const rotmg::Api& api, const std::string& json) {
                  return api.EvaluateChoices(json);
                }))
      .function("startSimulation",
                optional_override([](rotmg::Api& api, const std::string& json) { return api.StartSimulation(json); }))
      .function("advanceSimulation", &rotmg::Api::AdvanceSimulation)
      .function("simulateMany", optional_override([](const rotmg::Api& api, const std::string& json) {
                  return api.SimulateMany(json);
                }))
      .function("optimize", optional_override([](const rotmg::Api& api, const std::string& json) {
                  return api.Optimize(json);
                }))
      .function("optimizeWithProgress",
                optional_override([](const rotmg::Api& api, const std::string& json, emscripten::val report) {
                  return api.Optimize(json, [&](const std::string& progress) { report(progress); });
                }))
      .function("explainBuild", optional_override([](const rotmg::Api& api, const std::string& json) {
                  return api.ExplainBuild(json);
                }))
      .function("damageStats", optional_override([](const rotmg::Api& api, const std::string& json) {
                  return api.DamageStats(json);
                }))
      .function("estimateOptimize", optional_override([](const rotmg::Api& api, const std::string& json) {
                  return api.EstimateOptimize(json);
                }));
}
