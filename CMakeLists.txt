cmake_minimum_required(VERSION 3.5)

project(liblokimq CXX)

include(GNUInstallDirs)

set(LOKIMQ_VERSION_MAJOR 1)
set(LOKIMQ_VERSION_MINOR 0)
set(LOKIMQ_VERSION_PATCH 0)
set(LOKIMQ_VERSION "${LOKIMQ_VERSION_MAJOR}.${LOKIMQ_VERSION_MINOR}.${LOKIMQ_VERSION_PATCH}")
message(STATUS "lokimq v${LOKIMQ_VERSION}")

set(LOKIMQ_LIBVERSION 0)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_library(lokimq
    lokimq/auth.cpp
    lokimq/batch.cpp
    lokimq/bt_serialize.cpp
    lokimq/connections.cpp
    lokimq/jobs.cpp
    lokimq/lokimq.cpp
    lokimq/proxy.cpp
    lokimq/worker.cpp
)
set_target_properties(lokimq PROPERTIES SOVERSION ${LOKIMQ_LIBVERSION})

configure_file(lokimq/version.h.in lokimq/version.h @ONLY)

install(
    FILES lokimq/auth.h
          lokimq/batch.h
          lokimq/bt_serialize.h
          lokimq/connections.h
          lokimq/hex.h
          lokimq/lokimq.h
          lokimq/message.h
          lokimq/string_view.h
          ${CMAKE_CURRENT_BINARY_DIR}/lokimq/version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lokimq
)

# We require cppzmq >= 4.3, and versions before 4.3.2 have some security issues, so try to find
# >=4.3.2 on the system and, if we don't (or found an old one) build a bundled one.
include(FindPkgConfig)
pkg_search_module(libzmq QUIET libzmq>=4.3.2)
if(NOT libzmq_FOUND)
    message(STATUS "libzmq >= 4.3.2 not found, building bundled 4.3.2")
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/local-libzmq")
    include(LocalLibzmq)
endif()

set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "Disable cppzmq tests")
add_subdirectory(cppzmq EXCLUDE_FROM_ALL)

target_include_directories(lokimq
    PUBLIC
        $<INSTALL_INTERFACE:>
        $<INSTALL_INTERFACE:mapbox-variant/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/mapbox-variant/include>
)

target_compile_options(lokimq PRIVATE -Wall -Wextra -Werror)
set_target_properties(lokimq PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if(BUILD_SHARED_LIBS)
  target_link_libraries(lokimq PUBLIC cppzmq)
else()
  target_link_libraries(lokimq PUBLIC cppzmq-static)
endif()

if(NOT SODIUM_LIBRARIES)
    pkg_search_module(SODIUM REQUIRED libsodium)
endif()

# Really want to just use SODIUM_LINK_LIBRARIES here, but that isn't in older cmake:
find_library(libsodium_link_libs NAMES ${SODIUM_LIBRARIES} PATHS ${SODIUM_LIBRARY_DIRS})
target_link_libraries(lokimq PRIVATE ${libsodium_link_libs})

if(STATIC)
    target_link_libraries(cppzmq-static INTERFACE ${libsodium_link_libs})
endif()

target_include_directories(lokimq PRIVATE ${SODIUM_INCLUDE_DIRS})

add_library(lokimq::lokimq ALIAS lokimq)

export(
    TARGETS lokimq
    NAMESPACE lokimq::
    FILE lokimqTargets.cmake
)
install(
    TARGETS lokimq
    EXPORT lokimqConfig
    DESTINATION lib
)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  set(lokimq_IS_TOPLEVEL_PROJECT TRUE)
else()
  set(lokimq_IS_TOPLEVEL_PROJECT FALSE)
endif()

option(LOKIMQ_BUILD_TESTS "Building and perform lokimq tests" ${lokimq_IS_TOPLEVEL_PROJECT})
if(LOKIMQ_BUILD_TESTS)
    add_subdirectory(tests)
endif()

