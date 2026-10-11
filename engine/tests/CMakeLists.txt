include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.8.1
  GIT_SHALLOW TRUE
  SYSTEM)
FetchContent_MakeAvailable(Catch2)
include(Catch)

add_executable(rotmg_engine_tests
  schema_test.cpp
  dataset_test.cpp
  damage_test.cpp
  fire_rate_test.cpp
  evaluate_test.cpp
  wiki_test.cpp
  property_test.cpp
  api_test.cpp
  hits_test.cpp
  sim_test.cpp
  ability_test.cpp)
target_link_libraries(rotmg_engine_tests PRIVATE rotmg_engine rotmg_warnings Catch2::Catch2WithMain
                                                 nlohmann_json::nlohmann_json)
target_compile_definitions(rotmg_engine_tests PRIVATE ROTMG_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/../fixtures")
catch_discover_tests(rotmg_engine_tests)
