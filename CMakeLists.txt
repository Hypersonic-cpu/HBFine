cmake_minimum_required(VERSION 3.20)
project(HBFine VERSION 0.1.0 LANGUAGES CXX)

add_library(hbfine src/config.cpp src/controller.cpp)
add_library(HBFine::hbfine ALIAS hbfine)
target_compile_features(hbfine PUBLIC cxx_std_20)
target_include_directories(hbfine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
set_target_properties(hbfine PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hbfine PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(HBFINE_TEST_DEFAULT OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(HBFINE_TEST_DEFAULT ON)
endif()
option(HBFINE_BUILD_TESTS "Build standalone HBFine contract tests" ${HBFINE_TEST_DEFAULT})
if(HBFINE_BUILD_TESTS)
    enable_testing()
    add_executable(hbfine_tests tests/controller_tests.cpp)
    target_link_libraries(hbfine_tests PRIVATE HBFine::hbfine)
    add_test(NAME hbfine_device_contract COMMAND hbfine_tests)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS hbfine EXPORT HBFineTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/hbfine DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT HBFineTargets NAMESPACE HBFine:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HBFine)
configure_package_config_file(cmake/HBFineConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/HBFineConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HBFine)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/HBFineConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/HBFineConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/HBFineConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HBFine)
