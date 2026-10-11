cmake_minimum_required(VERSION 3.25)
project(rotmg_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ROTMG_BUILD_TESTS "Build the engine tests" ON)
option(ROTMG_WERROR "Treat warnings as errors" OFF)
option(ROTMG_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

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

add_library(rotmg_engine src/schema.cpp)
target_include_directories(rotmg_engine PUBLIC include)
target_link_libraries(rotmg_engine PRIVATE rotmg_warnings)

if(ROTMG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
