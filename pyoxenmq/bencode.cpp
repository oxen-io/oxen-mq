#include "common.hpp"
#include "oxenmq/base32z.h"

namespace oxenmq {

void BEncode_Init(py::module& mod) {
    mod.def("base32z_encode", [](py::bytes data) {
        char* ptr = nullptr;
        py::ssize_t sz = 0;
        PyBytes_AsStringAndSize(data.ptr(), &ptr, &sz);
        return oxenmq::to_base32z(ptr, ptr+sz);
    });
}

}
