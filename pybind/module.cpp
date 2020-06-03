#include "common.hpp"

PYBIND11_MODULE(pylokimq, m)
{
  lokimq::LokiMQ_Init(m);
}
