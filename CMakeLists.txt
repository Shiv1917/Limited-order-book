cmake_minimum_required(VERSION 3.20)
project(orderbook VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # needed for the Python module

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OB_BUILD_TESTS  "Build unit tests and fuzzers" ON)
option(OB_BUILD_BENCH  "Build benchmarks" ON)
option(OB_BUILD_TOOLS  "Build replay / backtest tools" ON)
option(OB_BUILD_PYTHON "Build pybind11 module (fetches pybind11)" OFF)
option(OB_NATIVE       "Compile with -march=native" ON)
option(OB_LIBFUZZER    "Build libFuzzer target (clang with libFuzzer runtime only)" OFF)
set(OB_SANITIZE "" CACHE STRING "Sanitizers, e.g. address,undefined or thread")

add_library(ob_warnings INTERFACE)
target_compile_options(ob_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

if(OB_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" OB_HAS_MARCH_NATIVE)
  if(OB_HAS_MARCH_NATIVE)
    add_compile_options(-march=native)
  else()
    check_cxx_compiler_flag("-mcpu=native" OB_HAS_MCPU_NATIVE)
    if(OB_HAS_MCPU_NATIVE)
      add_compile_options(-mcpu=native)
    endif()
  endif()
endif()

if(OB_SANITIZE)
  add_compile_options(-fsanitize=${OB_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${OB_SANITIZE})
endif()

# Record the exact flags so benchmark output can report them.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
get_directory_property(_opts COMPILE_OPTIONS)
string(REPLACE ";" " " _opts "${_opts}")
set(OB_FLAGS_STRING "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt}} ${_opts}")
string(STRIP "${OB_FLAGS_STRING}" OB_FLAGS_STRING)

# ---- core library -----------------------------------------------------------
add_library(ob_core
  src/types.cpp
  src/book.cpp
  src/env_info.cpp
  ref/ref_book.cpp)
target_include_directories(ob_core PUBLIC include ref tools)
target_compile_definitions(ob_core PRIVATE OB_BUILD_FLAGS="${OB_FLAGS_STRING}")
target_link_libraries(ob_core PRIVATE ob_warnings)
find_package(Threads REQUIRED)
target_link_libraries(ob_core PUBLIC Threads::Threads)

# ---- data tools -----------------------------------------------------------
add_library(ob_data
  tools/lobster.cpp
  tools/itch.cpp
  tools/msgfile.cpp
  strategy/market_maker.cpp)
target_include_directories(ob_data PUBLIC tools strategy)
target_link_libraries(ob_data PUBLIC ob_core PRIVATE ob_warnings)

if(OB_BUILD_TOOLS)
  add_executable(ob_replay tools/replay_main.cpp)
  target_link_libraries(ob_replay PRIVATE ob_data ob_warnings)
  add_executable(ob_synth tools/synth_main.cpp)
  target_link_libraries(ob_synth PRIVATE ob_data ob_warnings)
  add_executable(ob_itch2lobster tools/itch2lobster_main.cpp)
  target_link_libraries(ob_itch2lobster PRIVATE ob_data ob_warnings)
  add_executable(ob_mm_backtest strategy/backtest_main.cpp)
  target_link_libraries(ob_mm_backtest PRIVATE ob_data ob_warnings)
endif()

if(OB_BUILD_BENCH)
  add_executable(ob_bench bench/bench_main.cpp)
  target_link_libraries(ob_bench PRIVATE ob_data ob_warnings)
  add_executable(ob_e2e_bench bench/e2e_main.cpp)
  target_link_libraries(ob_e2e_bench PRIVATE ob_data ob_warnings)
endif()

if(OB_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)
  enable_testing()
  add_executable(ob_tests
    tests/test_bitset.cpp
    tests/test_flat_map.cpp
    tests/test_pool.cpp
    tests/test_book.cpp
    tests/test_fuzz.cpp
    tests/test_lobster.cpp
    tests/test_itch.cpp
    tests/test_spsc.cpp
    tests/test_histogram.cpp
    tests/test_market_maker.cpp)
  target_link_libraries(ob_tests PRIVATE ob_data Catch2::Catch2WithMain)
  target_compile_definitions(ob_tests PRIVATE OB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(Catch)
  catch_discover_tests(ob_tests)

  add_executable(ob_fuzz_diff tests/fuzz_diff.cpp)
  target_link_libraries(ob_fuzz_diff PRIVATE ob_data ob_warnings)
  add_test(NAME fuzz_diff_smoke COMMAND ob_fuzz_diff --seeds 20 --messages 20000)

  if(OB_LIBFUZZER)
    add_executable(ob_libfuzzer tests/libfuzzer_target.cpp)
    target_compile_options(ob_libfuzzer PRIVATE -fsanitize=fuzzer)
    target_link_options(ob_libfuzzer PRIVATE -fsanitize=fuzzer)
    target_link_libraries(ob_libfuzzer PRIVATE ob_core)
  endif()
endif()

if(OB_BUILD_PYTHON)
  include(FetchContent)
  find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)
  FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v3.0.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(pybind11)
  pybind11_add_module(pyob python/pyob.cpp)
  target_link_libraries(pyob PRIVATE ob_data)
endif()

option(OB_BUILD_SIM "Build the ob-sim event-driven backtester (fetches nlohmann/json)" ON)
if(OB_BUILD_SIM)
  add_subdirectory(sim)
endif()
