
add_subdirectory(Catch2)

set(LMQ_TEST_SRC
    main.cpp
    test_batch.cpp
    test_connect.cpp
    test_commands.cpp
    test_requests.cpp
    )

add_executable(tests ${LMQ_TEST_SRC})

find_package(Threads)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)

target_link_libraries(tests Catch2::Catch2 lokimq ${SODIUM_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})

set_property(TARGET tests PROPERTY CXX_STANDARD 14)
set_property(TARGET tests PROPERTY CXX_STANDARD_REQUIRED ON)

add_custom_target(check COMMAND tests)
