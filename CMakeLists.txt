cmake_minimum_required(VERSION 3.16)
project(asmopt C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(asmopt_lib
    src/asmopt.c
)
target_include_directories(asmopt_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(asmopt
    src/main.c
)
target_link_libraries(asmopt asmopt_lib)
