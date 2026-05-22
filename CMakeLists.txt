cmake_minimum_required(VERSION 3.10)

project(PED_CIC_CPU LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")

find_package(OpenMP)

add_executable(ped_cic_lsh_cpu
    src/main_cpu.cpp
    src/lsh_cpu.cpp
)

target_include_directories(ped_cic_lsh_cpu PRIVATE src)
if (OpenMP_CXX_FOUND)
    target_link_libraries(ped_cic_lsh_cpu PRIVATE OpenMP::OpenMP_CXX)
endif ()
