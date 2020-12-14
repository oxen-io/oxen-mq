cmake_minimum_required(VERSION 3.7)

# Has to be set before `project()`, and ignored on non-macos:
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.12 CACHE STRING "macOS deployment target (Apple clang only)")

project(liblokimq CXX C)

include(GNUInstallDirs)

set(LOKIMQ_VERSION_MAJOR 1)
set(LOKIMQ_VERSION_MINOR 2)
set(LOKIMQ_VERSION_PATCH 3)
set(LOKIMQ_VERSION "${LOKIMQ_VERSION_MAJOR}.${LOKIMQ_VERSION_MINOR}.${LOKIMQ_VERSION_PATCH}")
message(STATUS "lokimq v${LOKIMQ_VERSION}")

set(LOKIMQ_LIBVERSION 0)


if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(lokimq_IS_TOPLEVEL_PROJECT TRUE)
else()
    set(lokimq_IS_TOPLEVEL_PROJECT FALSE)
endif()


option(BUILD_SHARED_LIBS "Build shared libraries instead of static ones" ON)
set(lokimq_INSTALL_DEFAULT OFF)
if(BUILD_SHARED_LIBS OR lokimq_IS_TOPLEVEL_PROJECT)
    set(lokimq_INSTALL_DEFAULT ON)
endif()
option(LOKIMQ_BUILD_TESTS "Building and perform lokimq tests" ${lokimq_IS_TOPLEVEL_PROJECT})
option(LOKIMQ_INSTALL "Add lokimq libraries and headers to cmake install target; defaults to ON if BUILD_SHARED_LIBS is enabled or we are the top-level project; OFF for a static subdirectory build" ${lokimq_INSTALL_DEFAULT})
option(LOKIMQ_INSTALL_CPPZMQ "Install cppzmq header with lokimq/ headers (requires LOKIMQ_INSTALL)" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

configure_file(lokimq/version.h.in lokimq/version.h @ONLY)
configure_file(liblokimq.pc.in liblokimq.pc @ONLY)

add_library(lokimq
    lokimq/address.cpp
    lokimq/auth.cpp
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
# on a ton of other libraries, some of which are not all statically available.  If the caller wants
# to mess with this, so be it: they can set up a libzmq target and we'll use it.  Otherwise if they
# asked us to do things statically, don't even try to find a system lib and just build it.
set(lokimq_build_static_libzmq OFF)
if(TARGET libzmq)
    target_link_libraries(lokimq PUBLIC libzmq)
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

        target_link_libraries(lokimq PUBLIC PkgConfig::libzmq)
    else()
        set(lokimq_build_static_libzmq ON)
    endif()
else()
    set(lokimq_build_static_libzmq ON)
endif()

if(lokimq_build_static_libzmq)
    message(STATUS "libzmq >= 4.3 not found or static build requested, building bundled version")
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/local-libzmq")
    include(LocalLibzmq)
    target_link_libraries(lokimq PUBLIC libzmq_vendor)
endif()

target_include_directories(lokimq
    PUBLIC
        $<INSTALL_INTERFACE:>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cppzmq>
)

target_compile_options(lokimq PRIVATE -Wall -Wextra -Werror)
set_target_properties(lokimq PROPERTIES
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
    target_link_libraries(lokimq PUBLIC sodium)
    if(lokimq_build_static_libzmq)
        target_link_libraries(libzmq_vendor INTERFACE sodium)
    endif()
else()
    include(FindPkgConfig)
    pkg_check_modules(sodium REQUIRED libsodium IMPORTED_TARGET)

    if(BUILD_SHARED_LIBS)
        target_link_libraries(lokimq PUBLIC PkgConfig::sodium)
        if(lokimq_build_static_libzmq)
            target_link_libraries(libzmq_vendor INTERFACE PkgConfig::sodium)
        endif()
    else()
        link_dep_libs(lokimq PUBLIC "${sodium_STATIC_LIBRARY_DIRS}" ${sodium_STATIC_LIBRARIES})
        target_include_directories(lokimq PUBLIC ${sodium_STATIC_INCLUDE_DIRS})
        if(lokimq_build_static_libzmq)
            link_dep_libs(libzmq_vendor INTERFACE "${sodium_STATIC_LIBRARY_DIRS}" ${sodium_STATIC_LIBRARIES})
            target_link_libraries(libzmq_vendor INTERFACE ${sodium_STATIC_INCLUDE_DIRS})
        endif()
    endif()
endif()

add_library(lokimq::lokimq ALIAS lokimq)

export(
    TARGETS lokimq
    NAMESPACE lokimq::
    FILE lokimqTargets.cmake
)

if(LOKIMQ_INSTALL)
    install(
        TARGETS lokimq
        EXPORT lokimqConfig
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )

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
        lokimq/string_view.h
        lokimq/variant.h
        ${CMAKE_CURRENT_BINARY_DIR}/lokimq/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lokimq
        )

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
endif()

if(LOKIMQ_BUILD_TESTS)
    add_subdirectory(tests)
endif()
