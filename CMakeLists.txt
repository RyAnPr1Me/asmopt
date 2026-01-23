cmake_minimum_required(VERSION 3.16)
project(asmopt C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Enable testing
enable_testing()

add_library(asmopt_lib
    src/asmopt.c
)
target_include_directories(asmopt_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(asmopt
    src/main.c
)
target_link_libraries(asmopt asmopt_lib)

# Unit tests
add_executable(test_peephole
    tests/test_peephole.c
)
target_link_libraries(test_peephole asmopt_lib)
add_test(NAME PeepholeOptimizationTests COMMAND test_peephole)

# Integration tests
add_executable(test_integration
    tests/test_integration.c
)
target_link_libraries(test_integration asmopt_lib)
add_test(NAME IntegrationTests COMMAND test_integration)

# Comprehensive API and edge case tests
add_executable(test_comprehensive
    tests/test_comprehensive.c
)
target_link_libraries(test_comprehensive asmopt_lib)
add_test(NAME ComprehensiveTests COMMAND test_comprehensive)
