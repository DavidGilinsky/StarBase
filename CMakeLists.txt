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
    VERSION 0.1.7
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

# Target-name canonicalization (offline). Pure, no DB, so it stands alone.
add_library(sb_names STATIC src/names/canon.cpp)
target_include_directories(sb_names PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/names)
target_link_libraries(sb_names PUBLIC sb_warnings)

# ---- Optional dependency probing ----
if(SB_WITH_DB OR SB_WITH_FITS)
    find_package(PkgConfig REQUIRED)
endif()
if(SB_WITH_FITS)
    pkg_check_modules(CFITSIO REQUIRED cfitsio)
    find_package(OpenSSL REQUIRED)  # MD5 for the identity fingerprint
    message(STATUS "CFITSIO: ${CFITSIO_VERSION}")

    add_library(sb_fits STATIC src/fits/fits_reader.cpp src/fits/xisf_reader.cpp)
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
    target_link_libraries(sb_extract PUBLIC sb_fits sb_names)

    # Throughput benchmark (dev tool; not installed): measures header-read
    # concurrency scaling against a real archive to size the scanner's workers.
    add_executable(scan_bench tools/scan_bench.cpp)
    target_link_libraries(scan_bench PRIVATE sb_fits ${CFITSIO_LIBRARIES})
    target_include_directories(scan_bench PRIVATE ${CFITSIO_INCLUDE_DIRS})
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

    # Bridge between the database and the resolver: loads the header mapping
    # from the DB, and (later) stores resolved frames. Depends on both layers,
    # so neither sb_db nor sb_extract has to know about the other.
    if(SB_WITH_FITS)
        add_library(sb_index STATIC
            src/index/mapping_loader.cpp
            src/index/frame_store.cpp
            src/index/equipment.cpp)
        target_include_directories(sb_index PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/index)
        target_link_libraries(sb_index PUBLIC sb_db sb_extract)

        # Threaded directory sweep, sized by the scan_bench measurements.
        add_library(sb_scan STATIC src/scan/scanner.cpp)
        target_include_directories(sb_scan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/scan)
        target_link_libraries(sb_scan PUBLIC sb_index)

        # Vendored cpp-httplib + nlohmann/json (one config across all TUs).
        add_library(sb_httplib INTERFACE)
        target_include_directories(sb_httplib SYSTEM INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib)
        target_compile_definitions(sb_httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
        target_link_libraries(sb_httplib INTERFACE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

        # Filter-AST -> SQL query compiler (nlohmann/json in + the DB for escaping).
        add_library(sb_query STATIC src/query/query.cpp)
        target_include_directories(sb_query PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/query)
        target_link_libraries(sb_query PUBLIC sb_db sb_httplib)

        # Calibration matcher: darks/flats/bias/dark-flats for a light, ranked
        # and explained, driven by the declarative calibration_rules.
        add_library(sb_match STATIC src/match/calibration.cpp)
        target_include_directories(sb_match PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/match)
        target_link_libraries(sb_match PUBLIC sb_db sb_httplib)

        # Action engine: staging trees, filesystem ops (dry-run + trash), exports.
        add_library(sb_action STATIC src/action/action.cpp)
        target_include_directories(sb_action PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/action)
        target_link_libraries(sb_action PUBLIC sb_query)

        # PixInsight/WBPP bridge: render command-line automation from a staged
        # tree. Pure (no DB, no PixInsight), so it stands on its own.
        add_library(sb_pix STATIC src/pix/wbpp.cpp)
        target_include_directories(sb_pix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/pix)
        target_link_libraries(sb_pix PUBLIC sb_warnings)

        # Authentication: PBKDF2-HMAC-SHA256 password hashing and session tokens.
        add_library(sb_auth STATIC src/auth/password.cpp)
        target_include_directories(sb_auth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/auth)
        target_link_libraries(sb_auth PUBLIC sb_warnings OpenSSL::Crypto)

        # Optional online target-name resolution (CDS Sesame). Needs an HTTP
        # client, so it links httplib; the offline canonicalizer stays pure.
        add_library(sb_sesame STATIC src/names/sesame.cpp)
        target_include_directories(sb_sesame PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/names)
        target_link_libraries(sb_sesame PUBLIC sb_httplib sb_warnings)

        # Embedded HTTP/JSON API and static web UI, with self-signed TLS.
        add_library(sb_api STATIC src/api/http_server.cpp src/api/tls_cert.cpp)
        target_include_directories(sb_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/api)
        target_link_libraries(sb_api PUBLIC sb_scan sb_query sb_match sb_action sb_pix sb_auth sb_names sb_sesame sb_httplib OpenSSL::Crypto)
    endif()
endif()

# ---- starbased daemon ----
add_executable(starbased src/daemon/main.cpp)
if(SB_WITH_DB AND SB_WITH_FITS)
    target_link_libraries(starbased PRIVATE sb_api)
    target_compile_definitions(starbased PRIVATE SB_HAVE_API=1)
else()
    target_link_libraries(starbased PRIVATE sb_core)
endif()

# ---- starbasectl CLI ----
# Most commands talk to the database, so it is only built with the database
# layer. A -DSB_WITH_DB=OFF build still produces a working daemon. When the FITS
# extractor is also built, `starbasectl probe` reads and resolves a frame with
# no database connection at all.
if(SB_WITH_DB)
    add_executable(starbasectl src/cli/starbasectl.cpp)
    target_link_libraries(starbasectl PRIVATE sb_db)
    if(SB_WITH_FITS)
        target_link_libraries(starbasectl PRIVATE sb_scan)
        target_compile_definitions(starbasectl PRIVATE SB_HAVE_FITS=1)
    endif()
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

    # Target-name canonicalization is pure: no database.
    add_executable(canon_test tests/canon_test.cpp)
    target_link_libraries(canon_test PRIVATE sb_names)
    add_test(NAME canon_test COMMAND canon_test)

    if(SB_WITH_FITS)
        # Hermetic: writes a synthetic FITS and reads it back. Also checks a real
        # archive frame when SB_TEST_FITS points at one.
        add_executable(fits_test tests/fits_test.cpp)
        target_link_libraries(fits_test PRIVATE sb_fits ${CFITSIO_LIBRARIES})
        target_include_directories(fits_test PRIVATE ${CFITSIO_INCLUDE_DIRS})
        add_test(NAME fits_test COMMAND fits_test)

        # XISF parser builds its own test files; no archive, no database.
        add_executable(xisf_test tests/xisf_test.cpp)
        target_link_libraries(xisf_test PRIVATE sb_fits ${CFITSIO_LIBRARIES})
        target_include_directories(xisf_test PRIVATE ${CFITSIO_INCLUDE_DIRS})
        add_test(NAME xisf_test COMMAND xisf_test)

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

        if(SB_WITH_FITS)
            # Also DB-gated: loads seed.sql and checks the DB mapping matches the
            # compiled-in defaults, catching drift between the two.
            add_executable(mapping_test tests/mapping_test.cpp)
            target_link_libraries(mapping_test PRIVATE sb_index)
            add_test(NAME mapping_test COMMAND mapping_test)

            add_executable(index_test tests/index_test.cpp)
            target_link_libraries(index_test PRIVATE sb_index ${CFITSIO_LIBRARIES})
            target_include_directories(index_test PRIVATE ${CFITSIO_INCLUDE_DIRS})
            add_test(NAME index_test COMMAND index_test)

            add_executable(scan_test tests/scan_test.cpp)
            target_link_libraries(scan_test PRIVATE sb_scan ${CFITSIO_LIBRARIES})
            target_include_directories(scan_test PRIVATE ${CFITSIO_INCLUDE_DIRS})
            add_test(NAME scan_test COMMAND scan_test)

            add_executable(equipment_test tests/equipment_test.cpp)
            target_link_libraries(equipment_test PRIVATE sb_index)
            add_test(NAME equipment_test COMMAND equipment_test)

            add_executable(query_test tests/query_test.cpp)
            target_link_libraries(query_test PRIVATE sb_query)
            add_test(NAME query_test COMMAND query_test)

            add_executable(match_test tests/match_test.cpp)
            target_link_libraries(match_test PRIVATE sb_match)
            add_test(NAME match_test COMMAND match_test)

            add_executable(action_test tests/action_test.cpp)
            target_link_libraries(action_test PRIVATE sb_action)
            add_test(NAME action_test COMMAND action_test)

            # WBPP renderer is pure: it runs with no database and no PixInsight.
            add_executable(wbpp_test tests/wbpp_test.cpp)
            target_link_libraries(wbpp_test PRIVATE sb_pix)
            add_test(NAME wbpp_test COMMAND wbpp_test)

            # Password hashing is pure: no database needed.
            add_executable(password_test tests/password_test.cpp)
            target_link_libraries(password_test PRIVATE sb_auth)
            add_test(NAME password_test COMMAND password_test)
        endif()
    endif()
endif()

# ---- Install (self-contained bundle under CMAKE_INSTALL_PREFIX) ----
# Layout, flattened directly under the prefix:
#   <prefix>/bin  <prefix>/web  <prefix>/sql  <prefix>/share  <prefix>/lib/systemd/system
# The .deb's postinst reaches outside the prefix to symlink starbasectl onto
# PATH, link the systemd unit, seed /etc/starbase, and create the service user.
set(SB_SYMLINK_BINDIR "/usr/local/bin" CACHE PATH
    "Directory to symlink starbasectl into so it lands on PATH")

# Bake the real prefix into the systemd unit.
configure_file(config/systemd/starbased.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/starbased.service @ONLY)

install(TARGETS starbased RUNTIME DESTINATION bin)
if(SB_WITH_DB)
    install(TARGETS starbasectl RUNTIME DESTINATION bin)
endif()
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/starbase.conf.example
        DESTINATION share/starbase)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/starbased.service DESTINATION lib/systemd/system)
install(FILES sql/schema.sql sql/setup.sql sql/seed.sql sql/equipment.example.sql DESTINATION sql)
install(DIRECTORY sql/migrations DESTINATION sql OPTIONAL)
install(DIRECTORY web/ DESTINATION web OPTIONAL)
install(DIRECTORY pjsr/ DESTINATION share/starbase/pjsr OPTIONAL)
install(FILES LICENSE README.md DESTINATION share/doc/starbase)

# ---- Packaging (CPack: Debian .deb) ----
# Build one with:
#   cmake -B pkg -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build pkg --parallel
#   ( cd pkg && cpack -G DEB )
# The payload is the bundle under the prefix; the package's postinst creates the
# service user, the PATH/systemd symlinks, seeds /etc/starbase, applies the
# debconf answers, optionally creates the database, and enables the service.
set(CPACK_PACKAGE_NAME "starbase")
set(CPACK_PACKAGE_VENDOR "David Gilinsky")
set(CPACK_PACKAGE_CONTACT "David Gilinsky <gilinsky@gilinskyresearch.com>")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(CPACK_STRIP_FILES ON)

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "science")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)             # auto Depends from the ELF binaries
set(CPACK_DEBIAN_PACKAGE_DEPENDS "adduser, debconf (>= 0.5) | debconf-2.0")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "mariadb-server | mysql-server")
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)  # 0755 maintainer scripts
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")          # starbase_<ver>_<arch>.deb

# postinst/prerm/postrm carry @CMAKE_INSTALL_PREFIX@/@SB_SYMLINK_BINDIR@, so they
# are generated from .in; config/templates are static and shipped as-is.
configure_file(debian/postinst.in ${CMAKE_CURRENT_BINARY_DIR}/debian/postinst @ONLY)
configure_file(debian/prerm.in    ${CMAKE_CURRENT_BINARY_DIR}/debian/prerm    @ONLY)
configure_file(debian/postrm.in   ${CMAKE_CURRENT_BINARY_DIR}/debian/postrm   @ONLY)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_BINARY_DIR}/debian/postinst"
    "${CMAKE_CURRENT_BINARY_DIR}/debian/prerm"
    "${CMAKE_CURRENT_BINARY_DIR}/debian/postrm"
    "${CMAKE_CURRENT_SOURCE_DIR}/debian/config"
    "${CMAKE_CURRENT_SOURCE_DIR}/debian/templates")

include(CPack)

message(STATUS "StarBase ${PROJECT_VERSION}")
message(STATUS "  install prefix : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  database layer : ${SB_WITH_DB}")
message(STATUS "  FITS extractor : ${SB_WITH_FITS}")
message(STATUS "  tests          : ${SB_BUILD_TESTS}")
