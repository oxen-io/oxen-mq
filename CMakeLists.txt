cmake_minimum_required(VERSION 3.5)

project(liblokimq CXX)

add_library(lokimq
    lokimq/lokimq.cpp
    lokimq/bt_serialize.cpp
)

install(
    FILES lokimq/batch.h
          lokimq/bt_serialize.h
          lokimq/hex.h
          lokimq/lokimq.h
          lokimq/string_view.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lokimq
)

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
set_property(TARGET lokimq PROPERTY CXX_STANDARD 14)
set_property(TARGET lokimq PROPERTY CXX_STANDARD_REQUIRED ON)

target_link_libraries(lokimq PUBLIC cppzmq)

add_library(lokimq::lokimq ALIAS lokimq)

export(
    TARGETS lokimq
    NAMESPACE lokimq::
    FILE lokimqTargets.cmake
)
install(
    TARGETS lokimq
    EXPORT lokimqConfig
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

