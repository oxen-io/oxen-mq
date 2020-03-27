find_program(DOXYGEN doxygen)
if (NOT DOXYGEN)
    message(STATUS "Documentation generation disabled (doxygen not found)")
    return()
endif()
find_program(SPHINX_BUILD sphinx-build)
if (NOT DOXYGEN)
    message(STATUS "Documentation generation disabled (sphinx-build not found)")
    return()
endif()

set(lokimq_doc_sources
    ${PROJECT_SOURCE_DIR}/lokimq/auth.h
    ${PROJECT_SOURCE_DIR}/lokimq/batch.h
    ${PROJECT_SOURCE_DIR}/lokimq/bt_serialize.h
    ${PROJECT_SOURCE_DIR}/lokimq/connections.h
    ${PROJECT_SOURCE_DIR}/lokimq/hex.h
    ${PROJECT_SOURCE_DIR}/lokimq/lokimq.h
    ${PROJECT_SOURCE_DIR}/lokimq/message.h
    ${PROJECT_SOURCE_DIR}/lokimq/string_view.h
    ${PROJECT_BINARY_DIR}/lokimq/version.h
)
string(REPLACE ";" " " lokimq_doc_sources_spaced "${lokimq_doc_sources}")

add_custom_command(
    OUTPUT doxyxml/index.xml
    COMMAND ${DOXYGEN} Doxyfile
    DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        ${lokimq_doc_sources}
)

add_custom_command(
    OUTPUT html/index.html
    COMMAND ${SPHINX_BUILD}
        -Aversion=${LOKIMQ_VERSION} -Aversions=${LOKIMQ_VERSION_MAJOR},${LOKIMQ_VERSION_MINOR},${LOKIMQ_VERSION_PATCH}
        -b html
        ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/html
    DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/index.rst
        ${CMAKE_CURRENT_BINARY_DIR}/readme.rst
        ${CMAKE_CURRENT_BINARY_DIR}/conf.py
        ${CMAKE_CURRENT_BINARY_DIR}/doxyxml/index.xml
        ${PROJECT_SOURCE_DIR}/README.md
)
add_custom_target(doc DEPENDS html/index.html)

configure_file(Doxyfile.in Doxyfile @ONLY)
configure_file(conf.py.in conf.py @ONLY)
configure_file(index.rst index.rst COPYONLY)
configure_file(readme.rst.in readme.rst @ONLY)
