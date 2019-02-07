#include <ATen/core/dispatch/KernelRegistration.h>
#include "caffe2/operators/experimental/c10/schemas/flatten.h"
#include "caffe2/utils/math.h"
#include "caffe2/core/tensor.h"

using caffe2::BaseContext;
using caffe2::Tensor;

namespace caffe2 {
namespace {
template <class DataType, class Context>
void flatten_op_cpu_impl(
    const at::Tensor& input_,
    const at::Tensor& output_,
    int64_t axis) {
  Tensor input{C10Tensor(input_)};
  Tensor output{C10Tensor(output_)};
  CPUContext context;
  CAFFE_ENFORCE_GE(
      input.sizes().size(), axis, "The rank of the tensor must be >= axis.");
  output.Resize(input.size_to_dim(axis), input.size_from_dim(axis));
  context.CopyItemsSameDevice(
      input.dtype(),
      input.numel(),
      input.raw_data(),
      output.raw_mutable_data(input.dtype()));
}
} // namespace
} // namespace caffe2

namespace c10 {
C10_REGISTER_KERNEL(caffe2::ops::Flatten)
    .kernel<decltype(caffe2::flatten_op_cpu_impl<float, caffe2::CPUContext>), &caffe2::flatten_op_cpu_impl<float, caffe2::CPUContext>>()
    .dispatchKey(CPUTensorId());
} // namespace c10
