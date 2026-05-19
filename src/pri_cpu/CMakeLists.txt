cmake_minimum_required(VERSION 3.10)

project(FED_PRI_CPU LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(OpenMP)
enable_testing()

add_executable(pri_cpu_main
    pri_pipeline.cpp
    pri_cpu.cpp
)
target_include_directories(pri_cpu_main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pri_cpu_main PRIVATE OpenSSL::Crypto)
if (OpenMP_CXX_FOUND)
    target_link_libraries(pri_cpu_main PRIVATE OpenMP::OpenMP_CXX)
endif ()

add_executable(pri_cpu_benchmark
    pri_benchmark.cpp
)
target_include_directories(pri_cpu_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(pri_cpu_test
    pri_cpu_test.cpp
    pri_cpu.cpp
)
target_include_directories(pri_cpu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pri_cpu_test PRIVATE OpenSSL::Crypto)
add_test(NAME pri_cpu_test COMMAND pri_cpu_test)
