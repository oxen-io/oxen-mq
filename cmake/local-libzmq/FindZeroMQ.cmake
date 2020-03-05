
# cppzmq expects these targets:
add_library(libzmq ALIAS libzmq_vendor)
add_library(libzmq-static ALIAS libzmq_vendor)
set(ZeroMQ_FOUND ON)

