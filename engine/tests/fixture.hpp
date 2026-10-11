#pragma once

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "rotmg/dataset.hpp"

// fixtures/dataset-small.json, parsed. ROTMG_FIXTURES_DIR comes from CMake.
inline rotmg::Dataset LoadSmallDataset() {
  std::ifstream in(std::string(ROTMG_FIXTURES_DIR) + "/dataset-small.json", std::ios::binary);
  if (!in) {
    throw std::runtime_error("can't open fixtures/dataset-small.json");
  }
  std::ostringstream text;
  text << in.rdbuf();
  return rotmg::ParseDataset(text.str());
}
