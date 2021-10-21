#include "common.hpp"

PYBIND11_MODULE(pyoxenmq, m) {
    oxenmq::OxenMQ_Init(m);
    oxenmq::BEncode_Init(m);
}
