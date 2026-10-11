include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.8.1
  GIT_SHALLOW TRUE
  SYSTEM)
FetchContent_MakeAvailable(Catch2)
include(Catch)

add_executable(rotmg_engine_tests schema_test.cpp)
target_link_libraries(rotmg_engine_tests PRIVATE rotmg_engine rotmg_warnings Catch2::Catch2WithMain)
catch_discover_tests(rotmg_engine_tests)
