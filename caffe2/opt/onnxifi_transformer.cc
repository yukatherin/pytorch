#include "caffe2/opt/onnxifi_transformer.h"

#include <iostream>
#include <unordered_set>

#include "onnx/proto_utils.h"

#include "caffe2/core/context.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/operator.h"
#include "caffe2/core/tensor.h"
#include "caffe2/onnx/onnx_exporter.h"
#include "caffe2/opt/backend_cutting.h"
#include "caffe2/utils/proto_utils.h"

namespace caffe2 {

namespace {

using ShapeInfoMap = std::unordered_map<std::string, ShapeInfo>;

const std::string kNetPos("net_pos");
const std::string kModelId("model_id");
constexpr size_t kBufferSize = 64;

void AnnotateOpIndex(NetDef* net) {
  int i = 0;
  for (auto& op : *(net->mutable_op())) {
    AddArgument(kNetPos, i++, &op);
  }
}

std::string GetModelId(const NetDef& net) {
  static std::atomic<size_t> seq_id{0};
  auto model_id =
      ArgumentHelper(net).GetSingleArgument<std::string>("model_id", "");
  if (model_id.empty()) {
    model_id = "unnamed_" + c10::to_string(seq_id++);
  }
  return model_id;
}

// Convert ShapeInfo map to TensorShape map
std::unordered_map<std::string, TensorShape> StripShapeInfoMap(
    const ShapeInfoMap& info_map) {
  std::unordered_map<std::string, TensorShape> shape_map;
  for (const auto& kv : info_map) {
    shape_map.emplace(kv.first, kv.second.shape);
  }
  return shape_map;
}

// Wrap TensorShape into TensorProto
TensorProto WrapShapeInfoIntoTensorProto(
    const std::string& name,
    const ShapeInfo& shape_info) {
  TensorProto t;
  t.set_name(name);
  t.set_data_type(shape_info.shape.data_type());
  for (const auto i : shape_info.shape.dims()) {
    t.add_dims(i);
  }
  return t;
}

uint64_t OnnxifiDataType(caffe2::TensorProto::DataType t) {
#define CAFFE2_TO_ONNXIFI_TYPE(x, y) \
  case (caffe2::TensorProto::x):     \
    return y
  switch (t) {
    CAFFE2_TO_ONNXIFI_TYPE(FLOAT, ONNXIFI_DATATYPE_FLOAT32);
    CAFFE2_TO_ONNXIFI_TYPE(INT8, ONNXIFI_DATATYPE_INT8);
    CAFFE2_TO_ONNXIFI_TYPE(UINT8, ONNXIFI_DATATYPE_UINT8);
    CAFFE2_TO_ONNXIFI_TYPE(INT16, ONNXIFI_DATATYPE_INT16);
    CAFFE2_TO_ONNXIFI_TYPE(UINT16, ONNXIFI_DATATYPE_UINT16);
    CAFFE2_TO_ONNXIFI_TYPE(INT32, ONNXIFI_DATATYPE_INT32);
    CAFFE2_TO_ONNXIFI_TYPE(INT64, ONNXIFI_DATATYPE_INT64);
    CAFFE2_TO_ONNXIFI_TYPE(FLOAT16, ONNXIFI_DATATYPE_FLOAT16);
    default:
      LOG(WARNING) << "Unsupported Caffe2 tensor type: " << t
                   << ", fallback to FLOAT";
      return ONNXIFI_DATATYPE_FLOAT32;
  }
#undef CAFFE2_TO_ONNXIFI_TYPE
}

// TODO: Use ShapeInfo instead of shape
ShapeInfoMap InferShapes(
    Workspace* ws,
    NetDef* pred_net,
    CaffeMap<std::string, TensorShape>* shape_hints_ordered,
    bool infer_shapes,
    const BoundShapeSpec& spec) {
  ShapeInfoMap shape_map;
  if (infer_shapes) {
    // Populate shapes from workplace
    const std::vector<std::string> ws_blobs = ws->Blobs();
    for (const auto& s : ws_blobs) {
      auto shape = GetTensorShapeOfBlob(ws->GetBlob(s));
      if (!shape.unknown_shape()) {
        shape_map.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(s),
            std::forward_as_tuple(ShapeInfo::DimType::CONSTANT, shape));
      }
    }
    for (const auto& kv : *shape_hints_ordered) {
      shape_map.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(kv.first),
          std::forward_as_tuple(ShapeInfo::DimType::CONSTANT, kv.second));
    }
    BoundShapeInferencer eng(spec);
    eng.InferBoundShapeAndType(*pred_net, shape_map);
    const auto& out_map = eng.shape_info();

    for (const auto& kv : out_map) {
      shape_map.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(kv.first),
          std::forward_as_tuple(
              ShapeInfo::DimType::CONSTANT, kv.second.shape));
    }
  } else {
    // TODO: deprecate this path
    Workspace ws_local(ws);
    ws_local.RunNetOnce(*pred_net);
    const std::vector<std::string> ws_blobs = ws_local.Blobs();
    for (const auto& s : ws_blobs) {
      const Blob* b = ws_local.GetBlob(s);
      auto shape = GetTensorShapeOfBlob(b);
      if (!shape.unknown_shape()) {
        shape_map.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(s),
            std::forward_as_tuple(
                ShapeInfo::DimType::CONSTANT, std::move(shape)));
      }
    }
  }

  return shape_map;
}

std::vector<::ONNX_NAMESPACE::ValueInfoProto> ConvertToValueInfo(
    const std::vector<std::string>& names,
    const std::unordered_map<std::string, TensorShape>& shape_hints,
    const std::unordered_map<std::string, ::ONNX_NAMESPACE::TypeProto>&
        extra_shape_hints) {
  std::vector<::ONNX_NAMESPACE::ValueInfoProto> r;
  for (const auto& s : names) {
    r.emplace_back();
    auto& value_info = r.back();
    value_info.set_name(s);
    const auto it = shape_hints.find(s);
    if (it == shape_hints.end()) {
      const auto eit = extra_shape_hints.find(s);
      if (eit == extra_shape_hints.end()) {
        LOG(WARNING) << "Cannot get shape of " << s;
      } else {
        value_info.mutable_type()->CopyFrom(eit->second);
      }
    } else {
      auto* tensor_type = value_info.mutable_type()->mutable_tensor_type();
      tensor_type->set_elem_type(
          onnx::Caffe2TypeToOnnxType(it->second.data_type()));
      auto* shape = tensor_type->mutable_shape();
      for (int i = 0; i < it->second.dims().size(); ++i) {
        shape->add_dim()->set_dim_value(it->second.dims(i));
      }
    }
  }
  return r;
}

// Given a net, with primiary inputs and outputs defined in its
// external_inputs/outputs, and given the set of weights and extra weights
// (created during conversion to ONNX if exists), we check whether some of the
// weights are used in the net, and if so, we put it in the initialize_list and
// add it to the external_inputs too.
// \param net [in] c2 net (cutoff from a bigger net)
// \param weights_in_ws [in] all the weights in the workspace
// \param extra_weights [in] extra weights possibly generated during ONNX
// conversion \param initialization_list [out] weights that needs to be offload
// to backend \param total_inputs_vec [out] total #inputs of the net that
// doesn't have a producer
void GetWeightsAndInputs(
    const NetDef& net,
    const std::unordered_set<std::string>& weights_in_ws,
    const std::vector<std::string>& extra_weights,
    std::unordered_set<std::string>* initialization_list,
    std::vector<std::string>* total_inputs_vec) {
  std::unordered_set<std::string> total_inputs;

  // extra weights is definitely extra weights/inputs
  for (const auto& extra_weight : extra_weights) {
    if (total_inputs.emplace(extra_weight).second) {
      total_inputs_vec->emplace_back(extra_weight);
    }
    initialization_list->emplace(extra_weight);
  }

  // Boundary inputs that should not be weights
  std::unordered_set<std::string> boundary_inputs;
  for (const auto& i : net.external_input()) {
    boundary_inputs.emplace(i);
  }

  for (const auto& op : net.op()) {
    for (const auto& input : op.input()) {
      bool not_seen = total_inputs.emplace(input).second;
      if (!not_seen) {
        continue;
      }
      if (weights_in_ws.count(input)) {
        // We add weights as inputs too
        total_inputs_vec->emplace_back(input);
        initialization_list->emplace(input);
        VLOG(2) << "Add weights: " << input;
      } else if (boundary_inputs.count(input)) {
        VLOG(2) << "Adding boundary input: " << input;
        total_inputs_vec->emplace_back(input);
      }
    }
  }
}

void FillModelInfo(::ONNX_NAMESPACE::ModelProto* model) {
  model->set_ir_version(::ONNX_NAMESPACE::Version::IR_VERSION);
  model->set_producer_name("caffe2");
  auto* opset_id = model->add_opset_import();
  opset_id->set_domain("");
  opset_id->set_version(7);
}
} // namespace

OnnxifiTransformer::OnnxifiTransformer(const OnnxifiTransformerOptions& opts)
    : opts_(opts) {
  lib_ = onnx::initOnnxifiLibrary();
  CAFFE_ENFORCE(lib_, "Cannot initialize ONNXIFI library");
  CAFFE_ENFORCE_EQ(
      lib_->onnxGetBackendIDs(nullptr, &num_backends_),
      ONNXIFI_STATUS_FALLBACK);
  CAFFE_ENFORCE_GT(
      num_backends_, 0, "At least 1 onnxifi backend should be available");
  backend_ids_.resize(num_backends_);
  CAFFE_ENFORCE_EQ(
      lib_->onnxGetBackendIDs(backend_ids_.data(), &num_backends_),
      ONNXIFI_STATUS_SUCCESS);
}

OnnxifiTransformer::~OnnxifiTransformer() {
  for (unsigned i = 0; i < num_backends_; ++i) {
    if (lib_->onnxReleaseBackendID(backend_ids_[i]) != ONNXIFI_STATUS_SUCCESS) {
      LOG(ERROR) << "Error when calling onnxReleaseBackendID";
    }
  }
}

OperatorDef OnnxifiTransformer::BuildOnnxifiOp(
    const std::string& onnx_model_str,
    const std::unordered_map<std::string, TensorShape>& output_shape_hints,
    const std::unordered_set<std::string>& initialization_list,
    const caffe2::NetDef& net) {
  OperatorDef op;
  op.set_type("Onnxifi");
  auto* onnx_model_arg = op.add_arg();
  onnx_model_arg->set_name("onnx_model");
  onnx_model_arg->set_s(onnx_model_str);

  // Add the names of the initializer blobs that we want to fetch from the
  // workspace later
  auto* initializers_arg = op.add_arg();
  initializers_arg->set_name("initializers");
  for (const auto& s : initialization_list) {
    initializers_arg->add_strings(s);
    initializers_arg->add_strings(input_mapping_.at(s));
  }

  // Add the input/output
  auto* input_names = op.add_arg();
  input_names->set_name("input_names");
  for (const auto& input : net.external_input()) {
    if (!initialization_list.count(input)) {
      op.add_input(input);
      input_names->add_strings(input);
    }
  }
  auto* output_names = op.add_arg();
  output_names->set_name("output_names");
  for (const auto& output : net.external_output()) {
    op.add_output(output);
    output_names->add_strings(output);
  }

  // Add output size hints
  for (int i = 0; i < op.output_size(); ++i) {
    const auto& o = op.output(i);
    const auto it = output_shape_hints.find(o);
    if (it != output_shape_hints.end()) {
      const auto& shape = it->second;
      auto* output_shape_hint_arg = op.add_arg();
      output_shape_hint_arg->set_name(c10::str("output_shape_hint_", i));
      output_shape_hint_arg->add_ints(OnnxifiDataType(shape.data_type()));
      for (const auto& d : shape.dims()) {
        output_shape_hint_arg->add_ints(d);
      }

      VLOG(2) << "Adding output hint: " << o;
    }
  }

  // Tell Onnxifi op that the model is in onnx or c2 proto format
  AddArgument("use_onnx", opts_.use_onnx ? 1 : 0, &op);

  // Tell Onnxifi op which backend id to use
  AddArgument("backend_id", idx_, &op);

  // Add model_id and net_pos to the onnxifi model
  AddArgument(kModelId, model_id_, &op);
  AddArgument(kNetPos, c10::to_string(onnxifi_op_id_++), &op);

  return op;
}

NetDef OnnxifiTransformer::SubnetToOnnxifiOpViaC2(
    const caffe2::NetDef& net,
    const std::unordered_set<std::string>& weights_in_ws,
    const ShapeInfoMap& shape_hints) {
  // We already have all the ops and external inputs and outputs!
  NetDef wrapper_net(net);

  // Compute output shape hints
  std::unordered_map<std::string, TensorShape> output_shape_hints;
  for (const auto& o : wrapper_net.external_output()) {
    const auto it = shape_hints.find(o);
    CAFFE_ENFORCE(
        it != shape_hints.end(), "Cannot find shape info for output ", o);
    const auto& shape = it->second.shape;
    output_shape_hints.emplace(o, shape);
  }

  // Remove the second output of Concat from external_output. In addition, we
  // remove those outputs from the Onnxifi op too.
  // TODO: This approach is a bit hacky as we assume that the second output is
  // never used. A more appropriate approach can be learned from the ONNX path,
  // where we statically computes the split_info given input shape and insert a
  // GivenTensorIntFill op
  std::unordered_set<std::string> split_infos;
  NetDef net_copy(net);
  for (auto& op : *wrapper_net.mutable_op()) {
    if (op.type() == "Concat" && op.output_size() == 2) {
      split_infos.emplace(op.output(1));
    }
  }
  wrapper_net.clear_external_output();
  net_copy.clear_external_output();
  for (const auto& o : net.external_output()) {
    if (!split_infos.count(o)) {
      wrapper_net.add_external_output(o);
      net_copy.add_external_output(o);
    }
  }

  // Figure out weights and add it to external_inputs too
  std::vector<std::string> extra_weights;
  std::unordered_set<std::string> initialization_list;
  std::vector<std::string> total_inputs_vec;
  GetWeightsAndInputs(
      net,
      weights_in_ws,
      extra_weights,
      &initialization_list,
      &total_inputs_vec);
  auto* shape_arg = wrapper_net.add_arg();
  shape_arg->set_name("input_shape_info");
  wrapper_net.clear_external_input();
  for (const auto& i : total_inputs_vec) {
    wrapper_net.add_external_input(i);
    shape_arg->mutable_tensors()->Add()->CopyFrom(
        WrapShapeInfoIntoTensorProto(i, shape_hints.at(i)));
  }

  // Debugging stuff
  if (opts_.debug) {
    WriteProtoToTextFile(wrapper_net, "debug.pb_txt");
  }

  // C2 model is ready. Build ONNXIFI Op
  std::string model_str;
  wrapper_net.SerializeToString(&model_str);
  NetDef net_opt;
  auto* op = net_opt.add_op();
  *op = BuildOnnxifiOp(
      model_str, output_shape_hints, initialization_list, net_copy);
  for (const auto& i : op->input()) {
    net_opt.add_external_input(i);
  }
  for (const auto& o : op->output()) {
    net_opt.add_external_output(o);
  }
  return net_opt;
}

NetDef OnnxifiTransformer::SubnetToOnnxifiOpViaOnnx(
    const caffe2::NetDef& net,
    const std::unordered_set<std::string>& weights_in_ws,
    Workspace* ws,
    onnx::OnnxExporter* exporter,
    ShapeInfoMap* shape_hints,
    std::unordered_map<std::string, TensorShape>* shape_hints_onnx) {
  ::ONNX_NAMESPACE::ModelProto onnx_model;
  FillModelInfo(&onnx_model);

  // Convert c2 ops to onnx ops, add const weights if there are any
  DeviceOption option;
  CPUContext context(option);
  context.SwitchToDevice();
  std::vector<std::string> extra_weights;
  for (const auto& op : net.op()) {
    const auto results = exporter->Caffe2OpToOnnxNodes(op, *shape_hints_onnx);
    for (const auto& n : results.first) {
      onnx_model.mutable_graph()->add_node()->CopyFrom(n);
    }
    for (const auto& t : results.second) {
      VLOG(2) << "Adding extra init tensor: " << t.name();
      TensorShape shape;
      shape.mutable_dims()->CopyFrom(t.dims());
      auto ret = shape_hints_onnx->emplace(t.name(), std::move(shape));
      shape_hints->emplace(
          std::piecewise_construct,
          std::forward_as_tuple(ret.first->first),
          std::forward_as_tuple(
              ShapeInfo::DimType::CONSTANT, ret.first->second));

      // Feed into workspace as CPU Tensors
      auto* blob = ws->CreateBlob(t.name());
      auto* cpu_tensor = BlobGetMutableTensor(blob, CPU);
      std::vector<int64_t> dims;
      for(const auto& d : t.dims()) {
        dims.push_back(d);
      }
      cpu_tensor->Resize(dims);
      if (t.data_type() == ::ONNX_NAMESPACE::TensorProto::FLOAT) {
        context.CopyBytesSameDevice(
            cpu_tensor->numel() * sizeof(float),
            static_cast<const void*>(t.raw_data().data()),
            cpu_tensor->raw_mutable_data(TypeMeta::Make<float>()));
      } else if (t.data_type() == ::ONNX_NAMESPACE::TensorProto::INT64) {
        context.CopyBytesSameDevice(
            cpu_tensor->numel() * sizeof(int64_t),
            static_cast<const void*>(t.raw_data().data()),
            cpu_tensor->raw_mutable_data(TypeMeta::Make<int64_t>()));
      } else {
        CAFFE_THROW(
            "Unsupported tensor data type for conversion: ", t.data_type());
      }
      context.FinishDeviceComputation();

      // Add mappings
      extra_weights.emplace_back(t.name());
      CAFFE_ENFORCE(
          input_mapping_.emplace(t.name(), t.name()).second,
          c10::str("Tensor ", t.name(), " already exists in the workspace"));
    }
  }

  // Convert outputs and compute output shape hints
  std::vector<std::string> io_names;
  for (const auto& output : net.external_output()) {
    io_names.emplace_back(output);
  }
  auto io_vec = ConvertToValueInfo(
      io_names,
      *shape_hints_onnx,
      std::unordered_map<std::string, ::ONNX_NAMESPACE::TypeProto>());
  std::unordered_map<std::string, TensorShape> output_shape_hints;
  for (const auto& i : io_vec) {
    onnx_model.mutable_graph()->add_output()->CopyFrom(i);
    const auto it = shape_hints_onnx->find(i.name());
    CAFFE_ENFORCE(
        it != shape_hints_onnx->end(),
        "Cannot find shape info for output ",
        i.name());
    const auto& shape = it->second;
    output_shape_hints.emplace(i.name(), shape);
  }

  // Convert inputs and figure out weights
  std::unordered_set<std::string> initialization_list;
  std::vector<std::string> total_inputs_vec;
  GetWeightsAndInputs(
      net,
      weights_in_ws,
      extra_weights,
      &initialization_list,
      &total_inputs_vec);
  io_vec = ConvertToValueInfo(
      total_inputs_vec,
      *shape_hints_onnx,
      std::unordered_map<std::string, ::ONNX_NAMESPACE::TypeProto>());
  for (const auto& i : io_vec) {
    onnx_model.mutable_graph()->add_input()->CopyFrom(i);
  }

  // Debugging stuff
  if (opts_.debug) {
    WriteProtoToTextFile(onnx_model, "debug.onnx_txt");
  }

  // Onnx model is ready. Build ONNXIFI Op
  std::string model_str;
  onnx_model.SerializeToString(&model_str);
  NetDef net_opt;
  auto* op = net_opt.add_op();
  *op = BuildOnnxifiOp(model_str, output_shape_hints, initialization_list, net);
  for (const auto& i : op->input()) {
    net_opt.add_external_input(i);
  }
  for (const auto& i : op->output()) {
    net_opt.add_external_output(i);
  }

  return net_opt;
}

CaffeMap<std::string, TensorShape> OnnxifiTransformer::SsaRewriteAndMapNames(
    Workspace* ws,
    NetDef* pred_net,
    const std::unordered_map<std::string, TensorShape>& input_shape_hints) {
  input_mapping_ = onnx::SsaRewrite(nullptr, pred_net);
  // Annote the ops with net position
  AnnotateOpIndex(pred_net);
  std::vector<std::string> external_inputs;
  for (const auto kv : input_mapping_) {
    reverse_input_mapping_.emplace(kv.second, kv.first);
    if (!ws->HasBlob(kv.second)) {
      external_inputs.emplace_back(kv.first);
    }
  }
  for (const auto& i : external_inputs) {
    input_mapping_.erase(i);
  }
  CaffeMap<std::string, TensorShape> shape_hints_ordered;
  for (const auto& kv : input_shape_hints) {
    const auto it = reverse_input_mapping_.find(kv.first);
    if (it != reverse_input_mapping_.end()) {
      shape_hints_ordered.emplace(it->second, kv.second);
    } else {
      shape_hints_ordered.emplace(kv.first, kv.second);
    }
  }
  return shape_hints_ordered;
}

NetDef OnnxifiTransformer::TransformViaC2(
    NetDef* pred_net,
    const std::unordered_set<std::string>& weights,
    const std::unordered_set<int>& blacklisted_ops,
    const ShapeInfoMap& shape_hints) {
  onnxifi_library* backend = lib_;
  idx_ = 0;
  // Try to find a backend that support Caffe2 proto. Note that this is quite
  // opportunistic as we don't offcially support Caffe2 proto.
  if (!opts_.use_onnx) {
    char buf[kBufferSize];
    for (int i = 0; i < backend_ids_.size(); ++i) {
      size_t len = kBufferSize;
      auto ret = backend->onnxGetBackendInfo(
          backend_ids_[i], ONNXIFI_BACKEND_DEVICE, buf, &len);
      if (ret == ONNXIFI_STATUS_SUCCESS && strstr(buf, "Caffe2")) {
        LOG(INFO) << "Using backend with Caffe2 Proto, ID: " << i;
        idx_ = i;
        break;
      }
    }
  }
  onnxBackendID backend_id = backend_ids_[idx_];

  auto c2_supports = [&shape_hints, &blacklisted_ops, backend, backend_id](
                         const caffe2::OperatorDef& op) {
    try {
      int pos =
          ArgumentHelper::GetSingleArgument<OperatorDef, int>(op, kNetPos, -1);
      if (blacklisted_ops.count(pos)) {
        return false;
      }

      // Build a c2 net with one op
      NetDef net;
      net.add_op()->CopyFrom(op);
      for (const auto& i : op.input()) {
        net.add_external_input(i);
      }
      for (const auto& o : op.output()) {
        net.add_external_output(o);
      }
      // Remove the second output of Concat from the external_output
      if (op.type() == "Concat" && op.output_size() == 2) {
        net.mutable_external_output()->RemoveLast();
      }

      // Encode the input/output shapes to an argument
      auto* shape_arg = net.add_arg();
      shape_arg->set_name("input_shape_info");
      for (const auto& i : op.input()) {
        shape_arg->mutable_tensors()->Add()->CopyFrom(
            WrapShapeInfoIntoTensorProto(i, shape_hints.at(i)));
      }
      shape_arg = net.add_arg();
      shape_arg->set_name("output_shape_info");
      for (const auto& i : op.output()) {
        shape_arg->mutable_tensors()->Add()->CopyFrom(
            WrapShapeInfoIntoTensorProto(i, shape_hints.at(i)));
      }

      std::string c2_model_str;
      net.SerializeToString(&c2_model_str);
      auto ret = backend->onnxGetBackendCompatibility(
          backend_id, c2_model_str.size(), c2_model_str.c_str());
      if (ret != ONNXIFI_STATUS_SUCCESS) {
        LOG(INFO) << "Don't support c2 op " << op.type() << " (" << ret << ")";
        return false;
      } else {
        return true;
      }
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Caught exception when converting op " << op.type()
                 << ", what: " << ex.what();
      return false;
    }
  };

  auto c2_converter =
      [this, &weights, &shape_hints](const caffe2::NetDef& net) {
        return SubnetToOnnxifiOpViaC2(net, weights, shape_hints);
      };

  return opt::OptimizeForBackend(*pred_net, c2_supports, c2_converter);
}

NetDef OnnxifiTransformer::TransformViaOnnx(
    Workspace* ws,
    NetDef* pred_net,
    const std::unordered_set<std::string>& weights,
    const std::unordered_set<int>& blacklisted_ops,
    ShapeInfoMap* shape_hints) {
  onnxifi_library* backend = lib_;
  onnxBackendID backend_id = backend_ids_[0];
  auto shape_hints_onnx = StripShapeInfoMap(*shape_hints);

  // function to tell whether the ONNXIFI backend supports a given C2 op or not
  onnx::OnnxExporter exporter(nullptr);
  auto onnx_supports = [&exporter,
                        &shape_hints_onnx,
                        &blacklisted_ops,
                        backend,
                        backend_id](const caffe2::OperatorDef& op) {
    try {
      int pos =
          ArgumentHelper::GetSingleArgument<OperatorDef, int>(op, kNetPos, -1);
      if (blacklisted_ops.count(pos)) {
        return false;
      }
      const OpSchema* schema = OpSchemaRegistry::Schema(op.type());
      // NB: this might not be a hard constraint as we can just export C2
      // domain specific ops to ONNX
      if (!schema || schema->onnx_schema().empty()) {
        LOG(INFO) << "Cannot export c2 op " << op.type()
                  << " to onnx as there is no corresponding ONNX schema.";
        return false;
      }

      ::ONNX_NAMESPACE::ModelProto onnx_model;
      FillModelInfo(&onnx_model);
      auto results = exporter.Caffe2OpToOnnxNodes(op, shape_hints_onnx);
      std::unordered_set<std::string> used_inputs;
      std::unordered_set<std::string> used_outputs;
      std::vector<std::string> boundary_inputs;
      std::vector<std::string> boundary_outputs;
      std::unordered_set<std::string> reshape_info;
      // nodes are in topological order, so we just need to iterate
      for (const auto& n : results.first) {
        onnx_model.mutable_graph()->add_node()->CopyFrom(n);
        for (const auto& i : n.input()) {
          bool is_new = used_inputs.emplace(i).second;
          // The input is not seen and it's not referred by any nodes before as
          // output, we count it as an boudary input
          if (is_new && !used_outputs.count(i)) {
            boundary_inputs.emplace_back(i);
          }
        }
        for (const auto& o : n.output()) {
          used_outputs.emplace(o);
        }

        // For reshape node, if it has more than 1 inputs, we need to feed the
        // second input which contains the shape info
        if (n.op_type() == "Reshape" && n.input_size() > 1) {
          reshape_info.emplace(n.input(1));
        }
      }
      // Second iteration to account all the boundary outputs, which is a newly
      // seen output and is not referred as input before
      used_outputs.clear();
      for (const auto& n : results.first) {
        for (const auto& o : n.output()) {
          bool is_new = used_outputs.emplace(o).second;
          if (is_new && !used_inputs.count(o)) {
            boundary_outputs.emplace_back(o);
          }
        }
      }
      std::unordered_map<std::string, ::ONNX_NAMESPACE::TypeProto>
          extra_shape_hints;
      for (const auto& t : results.second) {
        extra_shape_hints.emplace(t.name(), onnx::ExtraTypeProto(t));
        if (reshape_info.count(t.name())) {
          onnx_model.mutable_graph()->add_initializer()->CopyFrom(t);
        }
      }

      // Add input/output shape info
      auto io_vec = ConvertToValueInfo(
          boundary_inputs, shape_hints_onnx, extra_shape_hints);
      for (const auto& i : io_vec) {
        onnx_model.mutable_graph()->add_input()->CopyFrom(i);
      }
      io_vec = ConvertToValueInfo(
          boundary_outputs, shape_hints_onnx, extra_shape_hints);
      for (const auto& i : io_vec) {
        onnx_model.mutable_graph()->add_output()->CopyFrom(i);
      }

      std::string onnx_model_str;
      onnx_model.SerializeToString(&onnx_model_str);
      auto ret = backend->onnxGetBackendCompatibility(
          backend_id, onnx_model_str.size(), onnx_model_str.c_str());
      if (ret != ONNXIFI_STATUS_SUCCESS) {
        LOG(INFO) << "Don't support onnx for " << op.type() << " c2 op (" << ret
                  << ")";
        return false;
      } else {
        return true;
      }
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Caught exception when converting op " << op.type()
                 << ", what: " << ex.what();
      return false;
    }
  };

  // function to convert runnable subgraph into an onnxifi op. We need to keep
  // the same exporter throughout the process to avoid duplicated dummy name
  // generation
  onnx::OnnxExporter exporter2(nullptr);
  auto onnx_converter =
      [this, ws, &weights, shape_hints, &shape_hints_onnx, &exporter2](
          const caffe2::NetDef& net) mutable {
        return SubnetToOnnxifiOpViaOnnx(
            net, weights, ws, &exporter2, shape_hints, &shape_hints_onnx);
      };

  return opt::OptimizeForBackend(*pred_net, onnx_supports, onnx_converter);
}

// Cutting off the runnable part and replace with ONNXIFI ops. Asssume the nets
// were topologically sorted
void OnnxifiTransformer::Transform(
    Workspace* ws,
    NetDef* pred_net,
    const std::vector<std::string>& external_inputs,
    const std::unordered_map<std::string, TensorShape>& input_shape_hints,
    const std::unordered_set<int>& blacklisted_ops) {
  CAFFE_ENFORCE(ws);
  CAFFE_ENFORCE(pred_net, "Predict net cannot be nullptr");

  // Get model id and  reset Onnxifi op id to 0
  model_id_ = GetModelId(*pred_net);
  onnxifi_op_id_ = 0;

  // SSA Rewrite the net
  auto shape_hints_ordered =
      SsaRewriteAndMapNames(ws, pred_net, input_shape_hints);

  // Populate shape info
  Workspace mapped_ws(ws, input_mapping_);
  ShapeInfoMap shape_hints = InferShapes(
      &mapped_ws,
      pred_net,
      &shape_hints_ordered,
      opts_.infer_shapes,
      opts_.bound_shape_spec);

  // Figure out what are the weights
  std::unordered_set<std::string> weights;
  std::unordered_set<std::string> input_set;
  for (const auto& i : external_inputs) {
    const auto it = reverse_input_mapping_.find(i);
    if (it != reverse_input_mapping_.end()) {
      input_set.emplace(it->second);
    }
  }
  const std::vector<string>& ws_blobs = mapped_ws.Blobs();
  for (const auto& s : ws_blobs) {
    if (!input_set.count(s)) {
      weights.emplace(s);
    }
  }

  // Transform the net
  NetDef net_opt = opts_.use_onnx
      ? TransformViaOnnx(ws, pred_net, weights, blacklisted_ops, &shape_hints)
      : TransformViaC2(pred_net, weights, blacklisted_ops, shape_hints);

  // Need to figure out a proper place to handle device option
  net_opt.mutable_device_option()->CopyFrom(pred_net->device_option());
  pred_net->Swap(&net_opt);
}

} // namespace caffe2
