cmake_minimum_required(VERSION 3.5)

project(liblokimq CXX)

set(lokimq_sources
    lokimq/lokimq.cpp
    lokimq/bt_serialize.cpp
    )

add_library(lokimq ${lokimq_sources})

add_library(lokimq-static STATIC EXCLUDE_FROM_ALL ${lokimq_sources})
set_target_properties(lokimq-static PROPERTIES OUTPUT_NAME lokimq)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq>=4.1)

add_subdirectory(cppzmq EXCLUDE_FROM_ALL)

foreach(target lokimq lokimq-static)
    target_include_directories(${target}
        PUBLIC
            $<INSTALL_INTERFACE:>
            $<INSTALL_INTERFACE:mapbox-variant/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/mapbox-variant/include>
    )

    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    set_property(TARGET ${target} PROPERTY CXX_STANDARD 14)
    set_property(TARGET ${target} PROPERTY CXX_STANDARD_REQUIRED ON)
endforeach()

target_link_libraries(lokimq PUBLIC cppzmq)
target_link_libraries(lokimq-static PUBLIC cppzmq-static)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

