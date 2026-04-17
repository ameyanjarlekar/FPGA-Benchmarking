cmake_minimum_required(VERSION 3.12)
project(host_benchmark CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

# Change this path to your repo location
add_subdirectory(/home/ameyasa2/xup_vitis_network_example/xrt_host_api xrt_host_api_build)

add_executable(host_benchmark host_benchmark.cpp)
target_include_directories(host_benchmark PRIVATE ${VNX_INCLUDE_PATH})
target_link_libraries(host_benchmark PRIVATE vnx)
