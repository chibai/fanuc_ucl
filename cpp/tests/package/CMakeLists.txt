cmake_minimum_required(VERSION 3.25)
project(snpx_package_consumer LANGUAGES CXX)

find_package(fanuc_snpx CONFIG REQUIRED)
add_executable(snpx_package_consumer main.cpp)
target_link_libraries(snpx_package_consumer PRIVATE fanuc_snpx::fanuc_snpx)
enable_testing()
add_test(NAME snpx_package_consumer COMMAND snpx_package_consumer)
