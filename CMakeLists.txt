cmake_minimum_required(VERSION 3.5)

project(liblokimq CXX)

include(GNUInstallDirs)

set(LOKIMQ_VERSION_MAJOR 1)
set(LOKIMQ_VERSION_MINOR 0)
set(LOKIMQ_VERSION_PATCH 0)
set(LOKIMQ_VERSION "${LOKIMQ_VERSION_MAJOR}.${LOKIMQ_VERSION_MINOR}.${LOKIMQ_VERSION_PATCH}")
message(STATUS "lokimq v${LOKIMQ_VERSION}")

set(LOKIMQ_LIBVERSION 0)


option(BUILD_SHARED_LIBS "Build shared libraries instead of static ones" ON)


list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

configure_file(lokimq/version.h.in lokimq/version.h @ONLY)
configure_file(liblokimq.pc.in liblokimq.pc @ONLY)

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

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(lokimq PRIVATE Threads::Threads)

# libzmq is nearly impossible to link statically from a system-installed static library: it depends
# on a ton of other libraries which typically are not all statically available.  So if the caller
# asked us to do things statically, don't even try to find a system lib and just build it.  If they
# really want it, they can set up the libzmq_FOUND etc. variables themselves.
if(BUILD_SHARED_LIBS)
  include(FindPkgConfig)
  pkg_search_module(libzmq QUIET libzmq>=4.3)
endif()
if(NOT libzmq_FOUND)
    message(STATUS "libzmq >= 4.3 not found, building bundled 4.3.2")
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/local-libzmq")
    include(LocalLibzmq)
endif()

set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "Disable cppzmq tests")
add_subdirectory(cppzmq EXCLUDE_FROM_ALL)

target_include_directories(lokimq
    PUBLIC
        $<INSTALL_INTERFACE:>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/mapbox-variant/include>
)

target_compile_options(lokimq PRIVATE -Wall -Wextra -Werror)
set_target_properties(lokimq PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

if(NOT SODIUM_LIBRARIES)
    pkg_search_module(SODIUM REQUIRED libsodium)
endif()

if(NOT ${CMAKE_VERSION} VERSION_LESS 3.13)
    # allow adding links to non-local targets (which is allowed even under old policy as long as the
    # target is an interface, but under new cmake it spews out warnings).
    cmake_policy(SET CMP0079 NEW)
endif()

function(link_dep_libs target linktype libdirs)
  foreach(lib ${ARGN})
    find_library(link_lib-${lib} NAMES ${lib} PATHS ${libdirs})
    if(link_lib-${lib})
      target_link_libraries(${target} ${linktype} ${link_lib-${lib}})
    endif()
  endforeach()
endfunction()

# Really want to just use SODIUM_LINK_LIBRARIES here, but that isn't in older cmake:
if(BUILD_SHARED_LIBS)
  link_dep_libs(lokimq PRIVATE ${SODIUM_LIBRARY_DIRS} ${SODIUM_LIBRARIES})
  target_include_directories(lokimq PRIVATE ${SODIUM_INCLUDE_DIRS})

  target_link_libraries(lokimq PUBLIC cppzmq)
else()
  link_dep_libs(lokimq PRIVATE "${SODIUM_STATIC_LIBRARY_DIRS}" ${SODIUM_STATIC_LIBRARIES})
  target_include_directories(lokimq PRIVATE ${SODIUM_STATIC_INCLUDE_DIRS})

  if (libzmq_FOUND)
    # cppzmq doesn't properly set up its libzmq target dependencies so do it here (this isn't the
    # normal case: we only get here if someone pre-set libzmq_FOUND etc. *and* wants static libs)
    link_dep_libs(cppzmq-static INTERFACE "${PC_LIBZMQ_STATIC_LIBRARY_DIRS}" ${PC_LIBZMQ_STATIC_LIBRARIES})
  else()
    # Otherwise we built a static vendored one, so make sure it links to the static libsodium
    link_dep_libs(libzmq_vendor INTERFACE "${SODIUM_STATIC_LIBRARY_DIRS}" ${SODIUM_STATIC_LIBRARIES})
  endif()

  target_link_libraries(lokimq PUBLIC cppzmq-static)
endif()

add_library(lokimq::lokimq ALIAS lokimq)

export(
    TARGETS lokimq
    NAMESPACE lokimq::
    FILE lokimqTargets.cmake
)
install(
    TARGETS lokimq
    EXPORT lokimqConfig
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

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
option(LOKIMQ_INSTALL_MAPBOX_VARIANT "Install mapbox-variant headers with lokimq/ headers" ON)
if(LOKIMQ_INSTALL_MAPBOX_VARIANT)
  install(
    FILES mapbox-variant/include/mapbox/variant.hpp
          mapbox-variant/include/mapbox/variant_cast.hpp
          mapbox-variant/include/mapbox/variant_io.hpp
          mapbox-variant/include/mapbox/variant_visitor.hpp
          mapbox-variant/include/mapbox/recursive_wrapper.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lokimq/mapbox
  )
endif()

option(LOKIMQ_INSTALL_CPPZMQ "Install cppzmq header with lokimq/ headers" ON)
if(LOKIMQ_INSTALL_CPPZMQ)
  install(
    FILES cppzmq/zmq.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lokimq
  )
endif()


install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/liblokimq.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
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

