#include "caffe2/operators/experimental/c10/schemas/concat.h"
#include <ATen/core/dispatch/OpSchemaRegistration.h>
#include "caffe2/core/operator_c10wrapper.h"

using caffe2::CPUContext;

namespace caffe2 {
namespace ops {
// TODO Parse schema string instead of creating FunctionSchema manually
C10_DEFINE_OP_SCHEMA(Concat, FunctionSchema(
    "_c10_experimental::Concat",
    (std::vector<c10::Argument>{
      c10::Argument("inputs", ListType::ofTensors()),
      c10::Argument("output"),
      c10::Argument("split_info", FloatType::get()),
      c10::Argument("add", IntType::get()),
      c10::Argument("add_axis", IntType::get())
    }), (std::vector<c10::Argument>{
    })
));
}
}

namespace {
struct AxisParameter final {
  using type = int;
  static constexpr const char* name() {
    return "axis";
  }
  static constexpr int default_value() {
    return -1;
  }
};
struct AddAxisParameter final {
  using type = int;
  static constexpr const char* name() {
    return "add_axis";
  }
  static constexpr int default_value() {
    return 0;
  }
};
} // namespace

namespace caffe2 {
REGISTER_C10_OPERATOR_FOR_CAFFE2_DISPATCH_WITH_ARRAY_INPUT_AND_PARAMETERS(
    ops::Concat,
    C10Concat_DontUseThisOpYet,
    2,
    ParameterHelper<AxisParameter>,
    ParameterHelper<AddAxisParameter>)
}
