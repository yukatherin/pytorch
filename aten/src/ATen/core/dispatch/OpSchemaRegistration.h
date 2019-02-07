#pragma once

#include <ATen/core/dispatch/Dispatcher.h>

namespace c10 {
namespace detail {
class OpSchemaRegistrar final {
public:
  explicit OpSchemaRegistrar(FunctionSchema schema)
  : opHandle_(c10::Dispatcher::singleton().registerSchema(std::move(schema))) {}

  ~OpSchemaRegistrar() {
    c10::Dispatcher::singleton().deregisterSchema(opHandle_);
  }

  const c10::OperatorHandle& opHandle() const {
    return opHandle_;
  }

private:
  c10::OperatorHandle opHandle_;
};
}  // namespace detail
}  // namespace c10

/**
 * Macro for defining an operator schema.  Every operator schema must
 * invoke C10_DECLARE_OP_SCHEMA in a header and C10_DEFINE_OP_SCHEMA in one (!)
 * cpp file.  Internally, this arranges for the dispatch table for
 * the operator to be created.
 */
#define C10_DECLARE_OP_SCHEMA(Name)                                             \
  CAFFE2_API const c10::OperatorHandle& Name();                                 \

#define C10_DEFINE_OP_SCHEMA(Name, Schema)                                      \
  C10_EXPORT const c10::OperatorHandle& Name() {                                \
    static ::c10::detail::OpSchemaRegistrar registrar(Schema);                  \
    return registrar.opHandle();                                                \
  }
