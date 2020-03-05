
# cppzmq expects these targets:
add_library(libzmq INTERFACE IMPORTED GLOBAL)
add_library(libzmq-static INTERFACE IMPORTED GLOBAL)
target_link_libraries(libzmq INTERFACE libzmq_vendor)
target_link_libraries(libzmq-static INTERFACE libzmq_vendor)
set(ZeroMQ_FOUND ON)

