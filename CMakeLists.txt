
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  foreach(lang C CXX)
    if(NOT DEFINED CMAKE_${lang}_COMPILER_LAUNCHER AND NOT CMAKE_${lang}_COMPILER MATCHES ".*/ccache")
      message(STATUS "Enabling ccache for ${lang}")
      set(CMAKE_${lang}_COMPILER_LAUNCHER ${CCACHE_PROGRAM} CACHE STRING "")
    endif()
  endforeach()
endif()

cmake_minimum_required(VERSION 3.7)

# Has to be set before `project()`, and ignored on non-macos:
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.12 CACHE STRING "macOS deployment target (Apple clang only)")

project(liboxenmq
    VERSION 1.2.6
    LANGUAGES CXX C)

include(GNUInstallDirs)

message(STATUS "oxenmq v${PROJECT_VERSION}")

set(OXENMQ_LIBVERSION 0)


if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(oxenmq_IS_TOPLEVEL_PROJECT TRUE)
else()
    set(oxenmq_IS_TOPLEVEL_PROJECT FALSE)
endif()


option(BUILD_SHARED_LIBS "Build shared libraries instead of static ones" ON)
set(oxenmq_INSTALL_DEFAULT OFF)
if(BUILD_SHARED_LIBS OR oxenmq_IS_TOPLEVEL_PROJECT)
    set(oxenmq_INSTALL_DEFAULT ON)
endif()
option(OXENMQ_BUILD_TESTS "Building and perform oxenmq tests" ${oxenmq_IS_TOPLEVEL_PROJECT})
option(OXENMQ_INSTALL "Add oxenmq libraries and headers to cmake install target; defaults to ON if BUILD_SHARED_LIBS is enabled or we are the top-level project; OFF for a static subdirectory build" ${oxenmq_INSTALL_DEFAULT})
option(OXENMQ_INSTALL_CPPZMQ "Install cppzmq header with oxenmq/ headers (requires OXENMQ_INSTALL)" ON)
option(OXENMQ_LOKIMQ_COMPAT "Install lokimq compatibility headers and pkg-config for rename migration" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

configure_file(oxenmq/version.h.in oxenmq/version.h @ONLY)
configure_file(liboxenmq.pc.in liboxenmq.pc @ONLY)
if(OXENMQ_LOKIMQ_COMPAT)
    configure_file(liblokimq.pc.in liblokimq.pc @ONLY)
endif()


add_library(oxenmq
    oxenmq/address.cpp
    oxenmq/auth.cpp
    oxenmq/bt_serialize.cpp
    oxenmq/connections.cpp
    oxenmq/jobs.cpp
    oxenmq/oxenmq.cpp
    oxenmq/proxy.cpp
    oxenmq/worker.cpp
)
set_target_properties(oxenmq PROPERTIES SOVERSION ${OXENMQ_LIBVERSION})

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(oxenmq PRIVATE Threads::Threads)

# libzmq is nearly impossible to link statically from a system-installed static library: it depends
# on a ton of other libraries, some of which are not all statically available.  If the caller wants
# to mess with this, so be it: they can set up a libzmq target and we'll use it.  Otherwise if they
# asked us to do things statically, don't even try to find a system lib and just build it.
set(oxenmq_build_static_libzmq OFF)
if(TARGET libzmq)
    target_link_libraries(oxenmq PUBLIC libzmq)
elseif(BUILD_SHARED_LIBS)
    include(FindPkgConfig)
    pkg_check_modules(libzmq libzmq>=4.3 IMPORTED_TARGET)

    if(libzmq_FOUND)
        # Debian sid includes a -isystem in the mit-krb package that, starting with pkg-config 0.29.2,
        # breaks cmake's pkgconfig module because it stupidly thinks "-isystem" is a path, so if we find
        # -isystem in the include dirs then hack it out.
        get_property(zmq_inc TARGET PkgConfig::libzmq PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
        list(FIND zmq_inc "-isystem" broken_isystem)
        if(NOT broken_isystem EQUAL -1)
          list(REMOVE_AT zmq_inc ${broken_isystem})
          set_property(TARGET PkgConfig::libzmq PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${zmq_inc})
        endif()

        target_link_libraries(oxenmq PUBLIC PkgConfig::libzmq)
    else()
        set(oxenmq_build_static_libzmq ON)
    endif()
else()
    set(oxenmq_build_static_libzmq ON)
endif()

if(oxenmq_build_static_libzmq)
    message(STATUS "libzmq >= 4.3 not found or static build requested, building bundled version")
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/local-libzmq")
    include(LocalLibzmq)
    target_link_libraries(oxenmq PUBLIC libzmq_vendor)
endif()

target_include_directories(oxenmq
    PUBLIC
        $<INSTALL_INTERFACE:>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cppzmq>
)

target_compile_options(oxenmq PRIVATE -Wall -Wextra -Werror)
set_target_properties(oxenmq PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

function(link_dep_libs target linktype libdirs)
    foreach(lib ${ARGN})
        find_library(link_lib-${lib} NAMES ${lib} PATHS ${libdirs})
        if(link_lib-${lib})
            target_link_libraries(${target} ${linktype} ${link_lib-${lib}})
        endif()
    endforeach()
endfunction()

# If the caller has already set up a sodium target then we will just link to it, otherwise we go
# looking for it.
if(TARGET sodium)
    target_link_libraries(oxenmq PUBLIC sodium)
    if(oxenmq_build_static_libzmq)
        target_link_libraries(libzmq_vendor INTERFACE sodium)
    endif()
else()
    include(FindPkgConfig)
    pkg_check_modules(sodium REQUIRED libsodium IMPORTED_TARGET)

    if(BUILD_SHARED_LIBS)
        target_link_libraries(oxenmq PUBLIC PkgConfig::sodium)
        if(oxenmq_build_static_libzmq)
            target_link_libraries(libzmq_vendor INTERFACE PkgConfig::sodium)
        endif()
    else()
        link_dep_libs(oxenmq PUBLIC "${sodium_STATIC_LIBRARY_DIRS}" ${sodium_STATIC_LIBRARIES})
        target_include_directories(oxenmq PUBLIC ${sodium_STATIC_INCLUDE_DIRS})
        if(oxenmq_build_static_libzmq)
            link_dep_libs(libzmq_vendor INTERFACE "${sodium_STATIC_LIBRARY_DIRS}" ${sodium_STATIC_LIBRARIES})
            target_link_libraries(libzmq_vendor INTERFACE ${sodium_STATIC_INCLUDE_DIRS})
        endif()
    endif()
endif()

add_library(oxenmq::oxenmq ALIAS oxenmq)
if(OXENMQ_LOKIMQ_COMPAT)
    add_library(lokimq::lokimq ALIAS oxenmq)
endif()

export(
    TARGETS oxenmq
    NAMESPACE oxenmq::
    FILE oxenmqTargets.cmake
)

if(OXENMQ_INSTALL)
    install(
        TARGETS oxenmq
        EXPORT oxenmqConfig
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )

    install(
        FILES oxenmq/address.h
        oxenmq/auth.h
        oxenmq/base32z.h
        oxenmq/base64.h
        oxenmq/batch.h
        oxenmq/bt_serialize.h
        oxenmq/bt_value.h
        oxenmq/byte_type.h
        oxenmq/connections.h
        oxenmq/hex.h
        oxenmq/oxenmq.h
        oxenmq/message.h
        oxenmq/variant.h
        ${CMAKE_CURRENT_BINARY_DIR}/oxenmq/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/oxenmq
        )

    if(OXENMQ_INSTALL_CPPZMQ)
        install(
            FILES cppzmq/zmq.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/oxenmq
            )
    endif()


    install(
        FILES ${CMAKE_CURRENT_BINARY_DIR}/liboxenmq.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )

    if(OXENMQ_LOKIMQ_COMPAT)
        install(
            FILES lokimq/address.h
            lokimq/auth.h
            lokimq/base32z.h
            lokimq/base64.h
            lokimq/batch.h
            lokimq/bt_serialize.h
            lokimq/bt_value.h
            lokimq/connections.h
            lokimq/hex.h
            lokimq/lokimq.h
            lokimq/message.h
            lokimq/variant.h
            lokimq/version.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lokimq
            )

        install(
            FILES ${CMAKE_CURRENT_BINARY_DIR}/liblokimq.pc
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
            )
    endif()


endif()

if(OXENMQ_BUILD_TESTS)
    add_subdirectory(tests)
endif()
