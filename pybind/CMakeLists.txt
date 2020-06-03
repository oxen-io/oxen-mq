pybind11_add_module(pylokimq MODULE
  lokimq.cpp
  module.cpp)
set_target_properties(pylokimq PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(pylokimq PUBLIC lokimq::lokimq)

install(
    TARGETS pylokimq
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
