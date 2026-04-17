cmake_minimum_required(VERSION 3.12)
project(host_benchmark CXX)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point this at the repo's xrt_host_api dir
add_subdirectory(/path/to/xup_vitis_network_example/xrt_host_api xrt_host_api_build)

add_executable(host_benchmark host_benchmark.cpp)
target_include_directories(host_benchmark PRIVATE ${VNX_INCLUDE_PATH})
target_link_libraries(host_benchmark PRIVATE vnx)
