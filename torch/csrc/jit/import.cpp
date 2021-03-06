#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/type_resolver_util.h>

#include "torch/csrc/jit/import.h"
#include "torch/csrc/jit/ir.h"
#include "torch/csrc/utils/functional.h"
#include "torch/csrc/jit/assertions.h"
#include "torch/csrc/jit/operator.h"
#include "torch/csrc/jit/import_method.h"


#include "caffe2/core/types.h"
#include "caffe2/proto/caffe2_pb.h"
#include "caffe2/proto/torch_pb.h"
#include "caffe2/serialize/inline_container.h"

#include <ATen/ATen.h>

#include <unordered_map>
#include <vector>
#include <string>
#include <fstream>

namespace torch { namespace jit {

namespace {

// this is a deserializer class which loads script modules from pt files. the
// content of the file is written using PyTorchStreamWriter, for details please
// check caffe2/serialize/inline_container.h. all the records except the last
// one are tensor data, and the last record is a serialized ModelProto, defined
// in caffe2/proto/torch.proto. ModelProto contains all the metadata of the
// model, and it is serialized as json.
class ScriptModuleDeserializer final {
 public:
  ScriptModuleDeserializer(const std::string& filename);

  ScriptModuleDeserializer(std::istream* is);

  void deserialize(ModuleLookup module_lookup);

private:
 at::Tensor loadTensor(
     const torch::TensorDef& tensor_proto,
     std::unordered_map<uint64_t, at::Storage>& storageMap);

 void convertModule(const torch::ModuleDef& module_def);

 void loadTensorTable(torch::ModelDef* model_def);

 std::ifstream ifs_;
 PyTorchStreamReader reader_;
 // this is a hack to make sure the script module created in C++ is the
 // same as created in Python
 ModuleLookup moduleLookup_;
 std::vector<std::string> moduleStack_;

 std::vector<at::Tensor> tensor_table_;
};

ScriptModuleDeserializer::ScriptModuleDeserializer(const std::string& filename)
    : ifs_(filename, std::ifstream::in | std::ifstream::binary),
      reader_(&ifs_) {
  // TODO appropriate support for mmap, right now still use stream reader
}

ScriptModuleDeserializer::ScriptModuleDeserializer(std::istream* is)
    : ifs_(), reader_(is) {}

void ScriptModuleDeserializer::deserialize(ModuleLookup module_lookup) {
  torch::ModelDef model_def;
  at::DataPtr data_ptr;
  size_t data_size;
  std::tie(data_ptr, data_size) = reader_.getLastRecord();
  // NB: cannot use JsonStringToMessage, since fbcode's protobuf is too old
  // be consistent with JsonStringToMessage
  std::string url_prefix = "type.googleapis.com";
  std::unique_ptr<::google::protobuf::util::TypeResolver> resolver(
      ::google::protobuf::util::NewTypeResolverForDescriptorPool(
          url_prefix, model_def.GetDescriptor()->file()->pool()));
  std::string json_string = std::string(
      static_cast<char*>(data_ptr.get()),
      static_cast<char*>(data_ptr.get()) + data_size);
  std::string binary_string;
  auto convert_result = ::google::protobuf::util::JsonToBinaryString(
      resolver.get(),
      url_prefix + "/" + model_def.GetDescriptor()->full_name(),
      json_string,
      &binary_string);
  if (!convert_result.ok()) {
    std::stringstream ss;
    ss << convert_result;
    AT_ERROR(ss.str());
  }
  AT_ASSERTM(
      model_def.ParseFromString(binary_string),
      "JSON transcoder produced invalid protobuf output.");
  moduleLookup_ = module_lookup;

  const auto& module_def = model_def.main_module();
  loadTensorTable(&model_def);
  // TODO: this can be simplified when C++/Python interop lands,
  // and the submodules would be created as the same in either C++ or Python
  convertModule(module_def);
}

void ScriptModuleDeserializer::loadTensorTable(torch::ModelDef* model_def) {
  std::unordered_map<uint64_t, at::Storage> storageMap;
  for(const torch::TensorDef& tensor : model_def->tensors()) {
    tensor_table_.emplace_back(loadTensor(tensor, storageMap));
  }
}

at::Tensor ScriptModuleDeserializer::loadTensor(const torch::TensorDef& tensor_proto,
                std::unordered_map<uint64_t, at::Storage>& storageMap) {
  std::vector<int64_t> dims(tensor_proto.dims().begin(), tensor_proto.dims().end());
  std::vector<int64_t> strides(tensor_proto.strides().begin(), tensor_proto.strides().end());
  auto type = at::typeMetaToScalarType(
      caffe2::DataTypeToTypeMeta(tensor_proto.data_type()));

  uint64_t record_id = caffe2::stoull(tensor_proto.data().key());
  auto storage_it = storageMap.find(record_id);
  if (storage_it == storageMap.end()) {
    at::DataPtr storage_ptr;
    uint64_t record_size;
    std::tie(storage_ptr, record_size) = reader_.getRecordWithKey(record_id);
    AT_ASSERT(record_size == tensor_proto.data().size());
    auto storage = at::Storage(
        at::CPU(type).typeMeta(),
        std::move(storage_ptr),
        record_size / at::CPU(type).typeMeta().itemsize(),
        nullptr); // NB: we didn't set any allocator for the tensor
    storage_it = storageMap.insert(std::make_pair(record_id, storage)).first;
  }
  auto t = at::CPU(type)._th_tensor(
      storage_it->second, tensor_proto.offset(), dims, strides);
  return autograd::make_variable(t, tensor_proto.requires_grad());
}

void ScriptModuleDeserializer::convertModule(
    const torch::ModuleDef& module_def) {
  std::shared_ptr<script::Module> module = moduleLookup_(moduleStack_);
  module->set_optimized(module_def.optimize());
  for (int i = 0; i < module_def.submodules_size(); ++i) {
    const torch::ModuleDef& sub_def = module_def.submodules(i);
    moduleStack_.emplace_back(sub_def.name());
    convertModule(sub_def);
    moduleStack_.pop_back();
  }
  for (int i = 0; i < module_def.parameters_size(); ++i) {
    const torch::ParameterDef& param_def = module_def.parameters(i);
    at::Tensor tensor = tensor_table_.at(param_def.tensor_id());
    module->register_parameter(
        param_def.name(), tensor, param_def.is_buffer());
  }
  at::DataPtr data;
  size_t size;
  std::tie(data, size) = reader_.getRecordWithKey(caffe2::stoull(module_def.torchscript_arena().key()));
  JIT_ASSERT(size == module_def.torchscript_arena().size());
  std::string data_str(static_cast<const char*>(data.get()), size);
  import_methods(module, data_str, tensor_table_);
}

}  // namespace

void import_ir_module(
    ModuleLookup module_lookup,
    std::istream& in) {
  ScriptModuleDeserializer deserializer(&in);
  deserializer.deserialize(module_lookup);
}

void import_ir_module(
    ModuleLookup module_lookup,
    const std::string& filename) {
  ScriptModuleDeserializer deserializer(filename);
  deserializer.deserialize(module_lookup);
}

std::shared_ptr<script::Module> load(std::istream& in) {
  auto module = std::make_shared<script::Module>();

  auto module_lookup = [&](const std::vector<std::string>& qualified_name) {
    std::shared_ptr<script::Module> curr = module;
    for (const auto& name : qualified_name) {
      if (curr->find_module(name) == nullptr) {
        curr->register_module(name, std::make_shared<script::Module>());
      }
      curr = curr->get_module(name);
    }
    return curr;
  };

  ScriptModuleDeserializer deserializer(&in);
  deserializer.deserialize(module_lookup);

  return module;
}

std::shared_ptr<script::Module> load(const std::string& filename) {
  std::ifstream in(filename, std::ios_base::binary);

  AT_CHECK(! in.fail(), "load: could not open file ", filename);

  auto module = load(in);

  return module;
}

}}
