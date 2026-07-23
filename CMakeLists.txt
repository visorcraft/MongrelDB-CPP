cmake_minimum_required(VERSION 3.16)

project(mongreldb_cpp
    VERSION 0.64.4
    DESCRIPTION "Header-only C++17 HTTP client for MongrelDB"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MONGRELDB_CPP_BUILD_TESTS "Build the MongrelDB C++ client tests" ON)

# libcurl is the only runtime dependency. On Debian/Ubuntu install
# libcurl4-openssl-dev; on Fedora curl-devel; on macOS it ships with the system.
find_package(CURL REQUIRED)

# A header-only library. The INTERFACE target carries the include path and the
# libcurl link requirement so consumers get both with target_link_libraries.
add_library(mongreldb_cpp INTERFACE)
target_include_directories(mongreldb_cpp
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(mongreldb_cpp INTERFACE CURL::libcurl)
target_compile_features(mongreldb_cpp INTERFACE cxx_std_17)

# src/mongreldb.cpp exists for users who prefer a compiled translation unit.
# It is empty by default because the client is header-only, but it is built
# into a convenience object so the include path and CURL link are exercised.
add_library(mongreldb_cpp_obj OBJECT src/mongreldb.cpp)
target_link_libraries(mongreldb_cpp_obj PUBLIC mongreldb_cpp)

if(MONGRELDB_CPP_BUILD_TESTS)
    enable_testing()
    add_executable(test_mongreldb tests/test_mongreldb.cpp)
    target_link_libraries(test_mongreldb PRIVATE mongreldb_cpp)
    add_test(NAME test_mongreldb COMMAND test_mongreldb)

    # Wire-shape conformance test for create_table payloads. This is a pure
    # unit test: it does not need a running daemon.
    add_executable(test_wire_shape tests/test_wire_shape.cpp)
    target_link_libraries(test_wire_shape PRIVATE mongreldb_cpp)
    add_test(NAME test_wire_shape COMMAND test_wire_shape)
endif()

# Install rules: headers only (the library is header-only).
include(GNUInstallDirs)

install(DIRECTORY include/mongreldb
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
)
