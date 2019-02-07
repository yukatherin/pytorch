#include <ATen/core/dispatch/KernelRegistration.h>
#include "caffe2/operators/experimental/c10/schemas/filler.h"
#include "caffe2/utils/math.h"
#include "caffe2/core/tensor.h"
#include <c10/core/Tensor.h>

using caffe2::CPUContext;
using caffe2::Tensor;
using caffe2::TensorCPU;
using std::vector;
using c10::ivalue::TensorList;

namespace caffe2 {
namespace {
void filler_init(
    ArrayRef<at::Tensor> inputs,
    const at::Tensor& output_,
    ArrayRef<int64_t> shape,
    ArrayRef<int64_t> extra_shape,
    bool input_as_shape) {
  Tensor output{C10Tensor(output_)};
  if (inputs.size()) {
    auto real_shape = vector<int64_t>{};
    if (input_as_shape) {
      // Shape input must be in CPU context
      Tensor input(inputs[0]);
      CAFFE_ENFORCE_EQ(
          input.dim(),
          1,
          "When input_as_shape is true, the input must be a 1D tensor of "
          "data type int64_t");
      auto* shape_data = input.template data<int64_t>();
      real_shape.insert(
          real_shape.end(), shape_data, shape_data + input.dim32(0));
    } else {
      Tensor input(inputs[0]);
      real_shape.insert(
          real_shape.end(), input.sizes().begin(), input.sizes().end());
    }
    real_shape.insert(real_shape.end(), extra_shape.begin(), extra_shape.end());
    output.Resize(real_shape);
  } else {
    output.Resize(shape);
  }
}

template <class Type, class Context>
void given_tensor_fill_op_cpu_impl(
    ArrayRef<at::Tensor> inputs,
    const at::Tensor& output_,
    ArrayRef<int64_t> shape,
    ArrayRef<int64_t> extra_shape,
    bool input_as_shape,
    const at::Tensor& values_) {
  Tensor output{C10Tensor(output_)};
  Tensor values{C10Tensor(values_)};
  CPUContext context;

  filler_init(inputs, output_, shape, extra_shape, input_as_shape);

  // TODO T might not be the correct type to call, since float allows others.

  DCHECK_EQ(output.numel(), values.numel())
      << "output size: " << output.numel()
      << " given size: " << values.numel();
  auto* data = output.template mutable_data<Type>();
  const Type* values_data = values.template data<Type>();
  if (output.numel()) {
    context.CopySameDevice(output.numel(), values_data, data);
  }
}

void constant_fill_op_cpu_impl(
    ArrayRef<at::Tensor> inputs,
    const at::Tensor& output_,
    ArrayRef<int64_t> shape,
    ArrayRef<int64_t> extra_shape,
    bool input_as_shape,
    int64_t dtype,
    c10::Scalar value) {
  Tensor output{C10Tensor(output_)};
  CPUContext context;

  filler_init(inputs, output_, shape, extra_shape, input_as_shape);

  if (output.numel()) {
    if (dtype == caffe2::TensorProto_DataType_FLOAT) {
      caffe2::math::Set<float, CPUContext>(
          output.numel(),
          value.toDouble(),
          output.template mutable_data<float>(),
          static_cast<CPUContext*>(&context));
    } else if (dtype == caffe2::TensorProto_DataType_INT32) {
      caffe2::math::Set<int32_t, CPUContext>(
          output.numel(),
          value.toInt(),
          output.template mutable_data<int32_t>(),
          static_cast<CPUContext*>(&context));
    } else if (dtype == caffe2::TensorProto_DataType_INT64) {
      caffe2::math::Set<int64_t, CPUContext>(
          output.numel(),
          value.toInt(),
          output.template mutable_data<int64_t>(),
          static_cast<CPUContext*>(&context));
    } else {
      throw std::logic_error(
          "Unimplemented data type for ConstantFill: " +
          c10::guts::to_string(dtype));
    }
  }
}

void uniform_fill_op_cpu_impl(
    ArrayRef<at::Tensor> inputs,
    const at::Tensor& output_,
    ArrayRef<int64_t> shape,
    ArrayRef<int64_t> extra_shape,
    bool input_as_shape,
    double min,
    double max) {
  Tensor output{C10Tensor(output_)};
  CPUContext context;

  filler_init(inputs, output_, shape, extra_shape, input_as_shape);

  if (inputs.size() == 3) {
    CAFFE_ENFORCE_EQ(1, Tensor(inputs[1]).numel(), "min blob must be scalar");
    CAFFE_ENFORCE_EQ(1, Tensor(inputs[2]).numel(), "max blob must be scalar");
    min = *Tensor(inputs[1]).template data<float>();
    max = *Tensor(inputs[2]).template data<float>();
    if (min > max) {
      auto shape = output.sizes().vec();
      shape[0] = 0;
      output.Resize(shape);
      output.template mutable_data<float>();
      return;
    }
  }
  caffe2::math::RandUniform<float, CPUContext>(
      output.numel(),
      min,
      max,
      output.template mutable_data<float>(),
      static_cast<CPUContext*>(&context));
}
} // namespace
} // namespace caffe2

namespace c10 {
C10_REGISTER_KERNEL(caffe2::ops::ConstantFill)
    .kernel<decltype(caffe2::constant_fill_op_cpu_impl), &caffe2::constant_fill_op_cpu_impl>()
    .dispatchKey(CPUTensorId());

C10_REGISTER_KERNEL(caffe2::ops::UniformFill)
    .kernel<decltype(caffe2::uniform_fill_op_cpu_impl), &caffe2::uniform_fill_op_cpu_impl>()
    .dispatchKey(CPUTensorId());

C10_REGISTER_KERNEL(caffe2::ops::GivenTensorFill)
    .kernel<decltype(caffe2::given_tensor_fill_op_cpu_impl<float, caffe2::CPUContext>), &caffe2::given_tensor_fill_op_cpu_impl<float, caffe2::CPUContext>>()
    .dispatchKey(CPUTensorId());

C10_REGISTER_KERNEL(caffe2::ops::GivenTensorIntFill)
    .kernel<decltype(caffe2::given_tensor_fill_op_cpu_impl<int, caffe2::CPUContext>), &caffe2::given_tensor_fill_op_cpu_impl<int, caffe2::CPUContext>>()
    .dispatchKey(CPUTensorId());

C10_REGISTER_KERNEL(caffe2::ops::GivenTensorInt64Fill)
    .kernel<decltype(caffe2::given_tensor_fill_op_cpu_impl<int64_t, caffe2::CPUContext>), &caffe2::given_tensor_fill_op_cpu_impl<int64_t, caffe2::CPUContext>>()
    .dispatchKey(CPUTensorId());
} // namespace c10
