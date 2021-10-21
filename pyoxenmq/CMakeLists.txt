cmake_minimum_required(VERSION 3.10)  # bionic's cmake version

set(PROJECT_NAME pyoxenmq)
project(${PROJECT_NAME}
    VERSION 1.0.0
    DESCRIPTION "python interface to OxenMQ"
    LANGUAGES CXX)

add_subdirectory(pybind11)

pybind11_add_module(pyoxenmq MODULE
    bencode.cpp
    oxenmq.cpp
    module.cpp
    )

target_compile_features(pyoxenmq PRIVATE cxx_std_17)

if(TARGET oxenmq::oxenmq)
    target_link_libraries(pyoxenmq PRIVATE oxenmq::oxenmq)
else()
    include(FindPkgConfig)
    pkg_check_modules(oxenmq REQUIRED IMPORTED_TARGET liboxenmq>=1.2.8)
    target_link_libraries(pyoxenmq PUBLIC PkgConfig::oxenmq)
endif()
