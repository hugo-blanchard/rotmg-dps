// rotmg-eval runs every request in a JSON array through the engine API and
// prints one response per line. CI runs the same requests through the
// WebAssembly build and compares the output.

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "rotmg/api.hpp"

namespace {

bool ReadFile(const char* path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream text;
  text << in.rdbuf();
  out = text.str();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: rotmg-eval <dataset.json> <requests.json>\n";
    return 2;
  }
  std::string dataset;
  std::string requests;
  if (!ReadFile(argv[1], dataset) || !ReadFile(argv[2], requests)) {
    std::cerr << "can't read " << argv[1] << " or " << argv[2] << "\n";
    return 1;
  }
  rotmg::Api api;
  if (std::string error = api.Load(dataset); !error.empty()) {
    std::cerr << error << "\n";
    return 1;
  }
  for (const nlohmann::json& request : nlohmann::json::parse(requests)) {
    std::cout << api.Evaluate(request.dump()) << "\n";
  }
  return 0;
}
