cmake_minimum_required(VERSION 3.25)
project(fanuc_snpx LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

add_library(fanuc_snpx src/client.cpp)
add_library(fanuc_snpx::fanuc_snpx ALIAS fanuc_snpx)
target_include_directories(fanuc_snpx PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(fanuc_snpx PUBLIC cxx_std_23)
target_link_libraries(fanuc_snpx PUBLIC Threads::Threads)
set_target_properties(fanuc_snpx PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(fanuc_snpx PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(fanuc_snpx PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(fanuc_snpx PUBLIC ws2_32)
endif()
if(BUILD_SHARED_LIBS AND WIN32)
    set_target_properties(fanuc_snpx PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

option(FANUC_SNPX_BUILD_EXAMPLES "Build SNP-X client example" ON)
if(FANUC_SNPX_BUILD_EXAMPLES)
    add_executable(snpx_example examples/snpx_example.cpp)
    target_link_libraries(snpx_example PRIVATE fanuc_snpx::fanuc_snpx)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(snpx_tests tests/protocol_tests.cpp)
    target_link_libraries(snpx_tests PRIVATE fanuc_snpx)
    add_test(NAME snpx_tests COMMAND snpx_tests)
    add_executable(snpx_client_integration tests/client_integration.cpp)
    target_link_libraries(snpx_client_integration PRIVATE fanuc_snpx)
    add_test(NAME snpx_client_integration COMMAND snpx_client_integration)
    set_tests_properties(snpx_tests snpx_client_integration PROPERTIES TIMEOUT 20)
endif()

install(TARGETS fanuc_snpx EXPORT fanuc_snpxTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/fanuc_snpx)
install(EXPORT fanuc_snpxTargets NAMESPACE fanuc_snpx::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fanuc_snpx)
configure_package_config_file(cmake/fanuc_snpxConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/fanuc_snpxConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fanuc_snpx)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fanuc_snpxConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fanuc_snpx)
