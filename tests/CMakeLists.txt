
add_subdirectory(Catch2)

set(LMQ_TEST_SRC
    main.cpp
    test_batch.cpp
    test_connect.cpp
    test_commands.cpp
    test_failures.cpp
    test_requests.cpp
    test_string_view.cpp
    )

add_executable(tests ${LMQ_TEST_SRC})

find_package(Threads)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)

target_link_libraries(tests Catch2::Catch2 lokimq ${SODIUM_LIBRARIES} Threads::Threads)

set_target_properties(tests PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

add_custom_target(check COMMAND tests)
