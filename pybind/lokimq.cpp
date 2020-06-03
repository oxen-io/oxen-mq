#include "common.hpp"
#include <lokimq/lokimq.h>
#include <future>
#include <memory>

namespace lokimq
{
  void
  LokiMQ_Init(py::module & mod)
  {
    py::class_<ConnectionID>(mod, "ConnectionID");
    
    py::class_<LokiMQ>(mod, "LokiMQ")
      .def(py::init<>())
      .def("start", &LokiMQ::start)
      .def("listen_plain",
           [](LokiMQ & self, std::string path) {
             self.listen_plain(path);
           })
      .def("listen_curve", &LokiMQ::listen_curve)
      .def("add_anonymous_category",
           [](LokiMQ & self, std::string name)
           {
             self.add_category(std::move(name), AuthLevel::none);
           })
      .def("add_request_command",
           [](LokiMQ &self,
              const std::string & category,
              const std::string & name,
              std::function<std::string(std::vector<std::string>)> handler)
           {
             self.add_request_command(category, name,
               [handler](Message & msg) {
                 std::vector<std::string> args;
                 for(const auto & arg : msg.data)
                   args.emplace_back(arg);
                 msg.send_reply(handler(std::move(args)));
               });
           })
      .def("connect_remote",
           [](LokiMQ & self,
              std::string remote) -> std::optional<ConnectionID>
           {
             std::promise<std::optional<ConnectionID>> promise;
             self.connect_remote(
               remote,
               [&promise](ConnectionID id) { promise.set_value(std::move(id)); },
               [&promise](auto, auto) { promise.set_value(std::nullopt); });
             return promise.get_future().get();
           })
      .def("request",
           [](LokiMQ & self,
              ConnectionID conn,
              std::string name,
              std::string arg) -> std::optional<std::vector<std::string>>
           {
             std::promise<std::optional<std::vector<std::string>>> result;
             self.request(conn, std::move(name),
                          [&result](bool success, std::vector<std::string> value)
                          {
                            if(not success)
                            {
                              result.set_value(std::nullopt);
                              return;
                            }
                            result.set_value(std::move(value));
                          }, arg);
             return result.get_future().get();
           });  
  }
}
