cmake_minimum_required(VERSION 3.14)
project(block3d VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenMP)

add_library(block3d
    src/core.cpp
    src/reader.cpp
    src/converter.cpp
)

target_include_directories(block3d PUBLIC include)
if(OpenMP_CXX_FOUND)
    target_link_libraries(block3d PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(block3d PRIVATE OMP_ENABLED)
endif()

if(MSVC)
    target_compile_options(block3d PRIVATE
        $<$<CONFIG:Release>:/arch:AVX2 /fp:fast>)
endif()

add_library(block3d_benchmark_cache
    src/benchmark_cache.cpp
)
target_include_directories(block3d_benchmark_cache PUBLIC include)

add_executable(block3d_cli src/cli.cpp)
target_link_libraries(block3d_cli PRIVATE block3d block3d_benchmark_cache)

enable_testing()

add_executable(test_block3d tests/test_block3d.cpp)
target_link_libraries(test_block3d PRIVATE block3d)
add_test(NAME test_block3d COMMAND test_block3d)

# CLI integration / regression test: drives the real block3d_cli binary
# (located next to this test executable in the same build output dir) to verify
# argument parsing and graceful error handling, not just the library API.
add_executable(test_cli tests/test_cli.cpp)
add_test(NAME test_cli COMMAND test_cli)

add_executable(run_test src/run_test.cpp)
target_link_libraries(run_test PRIVATE block3d block3d_benchmark_cache)

if(MSVC)
    set_target_properties(block3d block3d_benchmark_cache block3d_cli run_test test_block3d test_cli
        PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
