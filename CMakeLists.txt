
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
    VERSION 1.2.16
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
set(oxenmq_EPOLL_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    set(oxenmq_EPOLL_DEFAULT ON)
endif()

option(OXENMQ_BUILD_TESTS "Building and perform oxenmq tests" ${oxenmq_IS_TOPLEVEL_PROJECT})
option(OXENMQ_INSTALL "Add oxenmq libraries and headers to cmake install target; defaults to ON if BUILD_SHARED_LIBS is enabled or we are the top-level project; OFF for a static subdirectory build" ${oxenmq_INSTALL_DEFAULT})
option(OXENMQ_INSTALL_CPPZMQ "Install cppzmq header with oxenmq/ headers (requires OXENMQ_INSTALL)" ON)
option(OXENMQ_USE_EPOLL "Use epoll for socket polling (requires Linux)" ${oxenmq_EPOLL_DEFAULT})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

configure_file(oxenmq/version.h.in oxenmq/version.h @ONLY)
configure_file(liboxenmq.pc.in liboxenmq.pc @ONLY)


add_library(oxenmq
    oxenmq/address.cpp
    oxenmq/auth.cpp
    oxenmq/connections.cpp
    oxenmq/jobs.cpp
    oxenmq/oxenmq.cpp
    oxenmq/proxy.cpp
    oxenmq/worker.cpp
)
set_target_properties(oxenmq PROPERTIES SOVERSION ${OXENMQ_LIBVERSION})
if(OXENMQ_USE_EPOLL)
    target_compile_definitions(oxenmq PRIVATE OXENMQ_USE_EPOLL)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(oxenmq PRIVATE Threads::Threads)


if(TARGET oxenc::oxenc)
    add_library(_oxenmq_external_oxenc INTERFACE IMPORTED)
    target_link_libraries(_oxenmq_external_oxenc INTERFACE oxenc::oxenc)
    target_link_libraries(oxenmq PUBLIC _oxenmq_external_oxenc)
    message(STATUS "using pre-existing oxenc::oxenc target")
elseif(BUILD_SHARED_LIBS)
    include(FindPkgConfig)
    pkg_check_modules(oxenc liboxenc IMPORTED_TARGET)

    if(oxenc_FOUND)
        # Work around cmake bug 22180 (PkgConfig::tgt not set if no flags needed)
        if(TARGET PkgConfig::oxenc OR CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
            target_link_libraries(oxenmq PUBLIC PkgConfig::oxenc)
        endif()
    else()
        add_subdirectory(oxen-encoding)
        target_link_libraries(oxenmq PUBLIC oxenc::oxenc)
    endif()
else()
    add_subdirectory(oxen-encoding)
    target_link_libraries(oxenmq PUBLIC oxenc::oxenc)
endif()

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

target_compile_options(oxenmq PRIVATE -Wall -Wextra)

option(WARNINGS_AS_ERRORS "treat all warnings as errors" ON)
if(WARNINGS_AS_ERRORS)
    target_compile_options(oxenmq PRIVATE -Werror)
endif()

set_target_properties(oxenmq PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
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
        oxenmq/batch.h
        oxenmq/connections.h
        oxenmq/fmt.h
        oxenmq/message.h
        oxenmq/oxenmq.h
        oxenmq/pubsub.h
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

endif()

if(OXENMQ_BUILD_TESTS)
    add_subdirectory(tests)
endif()
