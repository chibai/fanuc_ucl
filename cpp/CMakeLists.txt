cmake_minimum_required(VERSION 3.25)
project(fanuc_snpx LANGUAGES CXX)

add_library(fanuc_snpx src/client.cpp)
target_include_directories(fanuc_snpx PUBLIC include)
target_compile_features(fanuc_snpx PUBLIC cxx_std_23)
set_target_properties(fanuc_snpx PROPERTIES CXX_EXTENSIONS OFF)

if(WIN32)
    target_link_libraries(fanuc_snpx PUBLIC ws2_32)
endif()

add_executable(snpx_example examples/snpx_example.cpp)
target_link_libraries(snpx_example PRIVATE fanuc_snpx)

include(CTest)
if(BUILD_TESTING)
    add_executable(snpx_tests tests/protocol_tests.cpp)
    target_link_libraries(snpx_tests PRIVATE fanuc_snpx)
    add_test(NAME snpx_tests COMMAND snpx_tests)
    add_executable(snpx_client_integration tests/client_integration.cpp)
    target_link_libraries(snpx_client_integration PRIVATE fanuc_snpx)
    add_test(NAME snpx_client_integration COMMAND snpx_client_integration)
endif()
