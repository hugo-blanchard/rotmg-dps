#pragma once

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "rotmg/dataset.hpp"

// fixtures/dataset-small.json. ROTMG_FIXTURES_DIR comes from CMake.
inline std::string ReadSmallDatasetText() {
  std::ifstream in(std::string(ROTMG_FIXTURES_DIR) + "/dataset-small.json", std::ios::binary);
  if (!in) {
    throw std::runtime_error("can't open fixtures/dataset-small.json");
  }
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

inline rotmg::Dataset LoadSmallDataset() {
  return rotmg::ParseDataset(ReadSmallDatasetText());
}
