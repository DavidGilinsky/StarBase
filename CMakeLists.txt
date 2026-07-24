# ---------------------------------------------------------------------------
# Author:        David Gilinsky
# File:          CMakeLists.txt
# Purpose:       Top-level CMake build: targets, options, install rules, tests.
# Created:       2026-07-23
# Last Modified: 2026-07-23
# Version:       0.1.0
# License:       GPL-3.0-or-later
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)

project(StarBase
    VERSION 0.1.0
    DESCRIPTION "A queryable interface to astronomical image files"
    HOMEPAGE_URL "https://github.com/DavidGilinsky/StarBase"
    LANGUAGES C CXX)

# ---- Language standards ----
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Install as a self-contained bundle: everything lives under one prefix so the
# install is clean and trivial to remove. An explicit -DCMAKE_INSTALL_PREFIX wins.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr/local/starbase" CACHE PATH
        "Install prefix (self-contained bundle)" FORCE)
endif()

option(SB_BUILD_TESTS "Build unit tests" ON)
option(SB_WITH_DB "Build the MariaDB database layer (requires libmariadb-dev)" ON)
option(SB_WITH_FITS "Build the FITS extractor (requires libcfitsio-dev)" ON)

find_package(Threads REQUIRED)

# ---- Shared warning flags ----
add_library(sb_warnings INTERFACE)
target_compile_options(sb_warnings INTERFACE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wall;-Wextra;-Wpedantic>)

# ---- Generated version header ----
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/starbase/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/starbase/version.hpp
    @ONLY)

# ---- Core library (config, logging, filesystem probing) ----
add_library(sb_core STATIC
    src/daemon/config.cpp
    src/daemon/logging.cpp
    src/util/fsinfo.cpp)
target_include_directories(sb_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/daemon
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(sb_core PUBLIC sb_warnings Threads::Threads)

# ---- Optional dependency probing ----
if(SB_WITH_DB OR SB_WITH_FITS)
    find_package(PkgConfig REQUIRED)
endif()
if(SB_WITH_FITS)
    pkg_check_modules(CFITSIO REQUIRED cfitsio)
    find_package(OpenSSL REQUIRED)  # MD5 for the identity fingerprint
    message(STATUS "CFITSIO: ${CFITSIO_VERSION}")

    add_library(sb_fits STATIC src/fits/fits_reader.cpp)
    target_include_directories(sb_fits PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fits
        ${CFITSIO_INCLUDE_DIRS})
    target_link_directories(sb_fits PUBLIC ${CFITSIO_LIBRARY_DIRS})
    target_link_libraries(sb_fits PUBLIC sb_core ${CFITSIO_LIBRARIES} OpenSSL::Crypto)

    # Header-to-frame resolver: raw cards -> normalized fields, via the mapping.
    add_library(sb_extract STATIC
        src/extract/resolver.cpp
        src/extract/resolver_defaults.cpp)
    target_include_directories(sb_extract PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/extract)
    target_link_libraries(sb_extract PUBLIC sb_fits)
endif()

# ---- Database layer (requires libmariadb-dev) ----
if(SB_WITH_DB)
    pkg_check_modules(MARIADB REQUIRED libmariadb)
    find_package(OpenSSL REQUIRED)
    message(STATUS "MariaDB Connector/C: ${MARIADB_VERSION}")

    add_library(sb_db STATIC src/db/database.cpp)
    target_include_directories(sb_db PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db
        ${MARIADB_INCLUDE_DIRS})
    target_link_directories(sb_db PUBLIC ${MARIADB_LIBRARY_DIRS})
    target_link_libraries(sb_db PUBLIC sb_core ${MARIADB_LIBRARIES})
endif()

# ---- starbased daemon ----
add_executable(starbased src/daemon/main.cpp)
target_link_libraries(starbased PRIVATE sb_core)

# ---- starbasectl CLI ----
# Every command it offers talks to the database, so it is only built with the
# database layer. A -DSB_WITH_DB=OFF build still produces a working daemon.
if(SB_WITH_DB)
    add_executable(starbasectl src/cli/starbasectl.cpp)
    target_link_libraries(starbasectl PRIVATE sb_db)
endif()

# ---- Generated example configuration ----
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config/starbase.conf.example.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/starbase.conf.example
    @ONLY)

# ---- Tests ----
if(SB_BUILD_TESTS)
    enable_testing()

    add_executable(config_test tests/config_test.cpp)
    target_link_libraries(config_test PRIVATE sb_core)
    add_test(NAME config_test COMMAND config_test)

    add_executable(fsinfo_test tests/fsinfo_test.cpp)
    target_link_libraries(fsinfo_test PRIVATE sb_core)
    add_test(NAME fsinfo_test COMMAND fsinfo_test)

    if(SB_WITH_FITS)
        # Hermetic: writes a synthetic FITS and reads it back. Also checks a real
        # archive frame when SB_TEST_FITS points at one.
        add_executable(fits_test tests/fits_test.cpp)
        target_link_libraries(fits_test PRIVATE sb_fits ${CFITSIO_LIBRARIES})
        target_include_directories(fits_test PRIVATE ${CFITSIO_INCLUDE_DIRS})
        add_test(NAME fits_test COMMAND fits_test)

        add_executable(resolver_test tests/resolver_test.cpp)
        target_link_libraries(resolver_test PRIVATE sb_extract ${CFITSIO_LIBRARIES})
        target_include_directories(resolver_test PRIVATE ${CFITSIO_INCLUDE_DIRS})
        add_test(NAME resolver_test COMMAND resolver_test)
    endif()

    if(SB_WITH_DB)
        # Skips itself (exit 0) unless SB_TEST_DB_NAME is set, so `ctest` is
        # useful on a machine with no database while CI still exercises it.
        add_executable(db_test tests/db_test.cpp)
        target_link_libraries(db_test PRIVATE sb_db)
        add_test(NAME db_test COMMAND db_test)
    endif()
endif()

# ---- Install ----
install(TARGETS starbased RUNTIME DESTINATION bin)
if(SB_WITH_DB)
    install(TARGETS starbasectl RUNTIME DESTINATION bin)
endif()
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/starbase.conf.example
        DESTINATION share/starbase)
install(FILES sql/schema.sql sql/setup.sql sql/seed.sql DESTINATION sql)
install(DIRECTORY sql/migrations DESTINATION sql OPTIONAL)
install(DIRECTORY web/ DESTINATION web OPTIONAL)
install(FILES LICENSE README.md DESTINATION share/doc/starbase)

message(STATUS "StarBase ${PROJECT_VERSION}")
message(STATUS "  install prefix : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  database layer : ${SB_WITH_DB}")
message(STATUS "  FITS extractor : ${SB_WITH_FITS}")
message(STATUS "  tests          : ${SB_BUILD_TESTS}")
