#include "common.hpp"
#include "lokimq/bt_serialize.h"
#include "lokimq/bt_value.h"

namespace pybind11::detail
{
  template <>
  struct type_caster<lokimq::bt_variant> : variant_caster<lokimq::bt_variant> { };
  
  /// this is because bt_value is really a bt_variant so we do some incantations to make type recursion work
  /// mostly stolen from variant's typecaster
  struct bt_value_typecaster
  {
    template <typename U>
    bool load_alternative(handle src, bool convert, type_list<U>) {
        auto caster = make_caster<U>();
        if (caster.load(src, convert)) {
            value = cast_op<U>(caster);
            return true;
        }
        return load_alternative(src, convert, type_list<lokimq::bt_variant>{});
    }

    bool load_alternative(handle, bool, type_list<>) { return false; }
    
    bool load(handle src, bool convert) {
        // Do a first pass without conversions to improve constructor resolution.
        // E.g. `py::int_(1).cast<variant<double, int>>()` needs to fill the `int`
        // slot of the variant. Without two-pass loading `double` would be filled
        // because it appears first and a conversion is possible.
      if (convert && load_alternative(src, false, type_list<lokimq::bt_variant>{}))
            return true;
        return load_alternative(src, convert, type_list<lokimq::bt_variant>{});
    }

    
    static handle cast(lokimq::bt_value && src, return_value_policy policy, handle parent) {
      return type_caster<lokimq::bt_variant>::cast(std::forward<lokimq::bt_variant>(src), policy, parent);
    }
    using Type = lokimq::bt_value;
    PYBIND11_TYPE_CASTER(Type, _("bt_value"));
  };

  template <>
  struct type_caster<lokimq::bt_value> : bt_value_typecaster { };
  
  
}

namespace lokimq
{
  
  void
  BEncode_Init(py::module & mod)
  {
    auto submod = mod.def_submodule("bencode", "bittorrent encoding/decoding module");
    submod.def("decode", [](std::string_view data) -> bt_value { return bt_deserialize<bt_value>(data); });
    submod.def("encode", [](bt_value stuff) -> std::string { return bt_serialize(stuff); });
  }
}
