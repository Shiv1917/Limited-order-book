# ob-sim: event-driven backtester on top of the order book (see docs/sim_design.md).
include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

add_library(ob_sim
  src/sim_core.cpp
  src/run.cpp
  src/sha256.cpp)
target_include_directories(ob_sim PUBLIC include)
target_link_libraries(ob_sim PUBLIC ob_data nlohmann_json::nlohmann_json PRIVATE ob_warnings)
target_compile_definitions(ob_sim PRIVATE
  OB_BUILD_FLAGS="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${CMAKE_BUILD_TYPE} ${OB_FLAGS_STRING}"
  OB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

add_executable(ob-sim src/main.cpp)
target_link_libraries(ob-sim PRIVATE ob_sim ob_warnings)

if(OB_BUILD_TESTS)
  add_executable(sim_tests
    tests/test_units.cpp
    tests/test_scenarios.cpp
    tests/test_properties.cpp)
  target_link_libraries(sim_tests PRIVATE ob_sim Catch2::Catch2WithMain)
  target_compile_definitions(sim_tests PRIVATE
    SIM_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios"
    OB_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
  catch_discover_tests(sim_tests TEST_PREFIX "sim:")
endif()
