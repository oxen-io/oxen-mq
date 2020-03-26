find_program(DOXYGEN doxygen)
if (NOT DOXYGEN)
    message(STATUS "Documentation generation disabled (doxygen not found)")
    return()
endif()

add_custom_target(doxygenxml
    COMMAND ${DOXYGEN} Doxyfile
    SOURCES Doxyfile
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/doxyxml/index.xml
)

add_custom_target(doc
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build.py ${LOKIMQ_VERSION}
    SOURCES
        ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        ${PROJECT_SOURCE_DIR}/lokimq/auth.h
        ${PROJECT_SOURCE_DIR}/lokimq/batch.h
        ${PROJECT_SOURCE_DIR}/lokimq/bt_serialize.h
        ${PROJECT_SOURCE_DIR}/lokimq/connections.h
        ${PROJECT_SOURCE_DIR}/lokimq/hex.h
        ${PROJECT_SOURCE_DIR}/lokimq/lokimq.h
        ${PROJECT_SOURCE_DIR}/lokimq/message.h
        ${PROJECT_SOURCE_DIR}/lokimq/string_view.h
)

configure_file(Doxyfile.in Doxyfile @ONLY)
configure_file(conf.py.in conf.py @ONLY)
