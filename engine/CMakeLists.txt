cmake_minimum_required(VERSION 3.25)
project(rotmg_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Tests and tools are native only; an Emscripten build makes engine.js and
# engine.wasm for the web app.
if(EMSCRIPTEN)
  set(native_default OFF)
else()
  set(native_default ON)
endif()
option(ROTMG_BUILD_TESTS "Build the engine tests" ${native_default})
option(ROTMG_BUILD_TOOLS "Build the rotmg-dps and rotmg-eval tools" ${native_default})
option(ROTMG_WERROR "Treat warnings as errors" OFF)
option(ROTMG_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

if(EMSCRIPTEN)
  # The dataset loader reports errors with exceptions, and the optimizer
  # runs on several threads.
  add_compile_options(-fwasm-exceptions -pthread)
  add_link_options(-fwasm-exceptions -pthread)
else()
  find_package(Threads REQUIRED)
endif()

if(ROTMG_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(rotmg_warnings INTERFACE)
if(MSVC)
  target_compile_options(rotmg_warnings INTERFACE /W4 $<$<BOOL:${ROTMG_WERROR}>:/WX>)
else()
  target_compile_options(rotmg_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion
                                                  $<$<BOOL:${ROTMG_WERROR}>:-Werror>)
endif()

include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  SYSTEM)
FetchContent_MakeAvailable(nlohmann_json)

add_library(rotmg_engine src/schema.cpp src/dataset.cpp src/damage.cpp src/fire_rate.cpp src/evaluate.cpp src/api.cpp src/request.cpp src/hits.cpp src/path.cpp src/sim.cpp src/ability.cpp src/build.cpp src/sets.cpp src/enchant.cpp src/optimize.cpp src/parallel.cpp
            src/search/model.cpp src/search/score.cpp src/search/enchantments.cpp)
target_include_directories(rotmg_engine PUBLIC include)
target_link_libraries(rotmg_engine PRIVATE rotmg_warnings nlohmann_json::nlohmann_json)
if(NOT EMSCRIPTEN)
  target_link_libraries(rotmg_engine PUBLIC Threads::Threads)
endif()

if(ROTMG_BUILD_TOOLS)
  add_executable(rotmg-dps tools/dps.cpp)
  target_link_libraries(rotmg-dps PRIVATE rotmg_engine rotmg_warnings)
  add_executable(rotmg-eval tools/eval.cpp)
  target_link_libraries(rotmg-eval PRIVATE rotmg_engine rotmg_warnings nlohmann_json::nlohmann_json)
endif()

if(EMSCRIPTEN)
  add_executable(engine wasm/bindings.cpp)
  target_link_libraries(engine PRIVATE rotmg_engine rotmg_warnings)
  target_link_options(engine PRIVATE
    -lembind
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createEngine
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    # Workers for the optimizer's threads, started as the engine loads: at
    # least as many as src/parallel.cpp's kMaxThreads helpers.
    -sPTHREAD_POOL_SIZE=8
    # The default allocator takes one lock for every thread, and evaluating
    # a build allocates a lot; mimalloc keeps a heap per thread.
    -sMALLOC=mimalloc)
endif()

if(ROTMG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
