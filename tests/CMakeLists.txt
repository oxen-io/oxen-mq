
add_subdirectory(Catch2)

set(LMQ_TEST_SRC
    main.cpp
    test_batch.cpp
    test_commands.cpp
    test_requests.cpp
    )

add_executable(tests ${LMQ_TEST_SRC})

target_link_libraries(tests Catch2::Catch2 lokimq)

add_custom_target(check COMMAND tests)
