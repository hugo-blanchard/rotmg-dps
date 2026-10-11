// Exposes rotmg::Api to JavaScript. The web app runs it in a Web Worker.

#include <string>

#include <emscripten/bind.h>

#include "rotmg/api.hpp"

EMSCRIPTEN_BINDINGS(rotmg) {
  using emscripten::optional_override;
  emscripten::class_<rotmg::Api>("Api")
      .constructor<>()
      .function("load", optional_override([](rotmg::Api& api, const std::string& json) { return api.Load(json); }))
      .function("evaluate",
                optional_override([](const rotmg::Api& api, const std::string& json) { return api.Evaluate(json); }));
}
