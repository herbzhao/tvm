#include "vsi_npu_runtime.h"

#include <dmlc/memory_io.h>
#include <tvm/runtime/c_runtime_api.h>
#include <tvm/runtime/memory.h>
#include <tvm/runtime/module.h>
#include <tvm/runtime/object.h>
#include <tvm/runtime/packed_func.h>
#include <tvm/runtime/registry.h>

#include <tim/vx/operation.h>
#include <tim/vx/tensor.h>
#include <tim/vx/ops/nbg.h>

#include <sys/time.h>
#include <time.h>

#define BILLION                                 1000000000
static uint64_t get_perf_count()
{
#if defined(__linux__) || defined(__ANDROID__) || defined(__QNX__) || defined(__CYGWIN__)
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)((uint64_t)ts.tv_nsec + (uint64_t)ts.tv_sec * BILLION);
#elif defined(_WIN32) || defined(UNDER_CE)
    LARGE_INTEGER ln;

    QueryPerformanceCounter(&ln);

    return (uint64_t)ln.QuadPart;
#endif
}

namespace tvm {
namespace runtime {
namespace vsi_npu {
PackedFunc VsiNpuModule::GetFunction(const std::string& name, const ObjectPtr<Object>& sptr_to_self) {
  std::string vsi_graph_name = "tvmgen_default_vsi_npu_0";
  std::cout << "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDebug " << name << std::endl;
  if (name != vsi_graph_name){
  std::cout << "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDebug return early" << std::endl;
    return PackedFunc();
  }

  // TODO: only handle name = "vsi_npu_*", but now we can not see vsi_npu_* invoked
  return PackedFunc([sptr_to_self, this, name](TVMArgs args, TVMRetValue* rv) {
    this->vx_context_ = tim::vx::Context::Create();
    this->vx_graph_ = this->vx_context_->CreateGraph();

    // auto nbg_node = std::make_shared<tim::vx::Operation>(
    //     this->vx_graph_.get(), 68, this->inputs_.size(), this->outputs_.size());
    auto nbg_node = this->vx_graph_->CreateOperation<tim::vx::ops::NBG>(
        this->compiled_nbg_.get(), this->inputs_.size(), this->outputs_.size());

    std::vector<std::shared_ptr<tim::vx::Tensor>> inputs, outputs;

    std::transform(
        this->inputs_.begin(), this->inputs_.end(), std::back_inserter(inputs),
        [this](const tim::vx::TensorSpec& spec) { return this->vx_graph_->CreateTensor(spec); });

    std::transform(
        this->outputs_.begin(), this->outputs_.end(), std::back_inserter(outputs),
        [this](const tim::vx::TensorSpec& spec) { return this->vx_graph_->CreateTensor(spec); });

    (*nbg_node).BindInputs(inputs).BindOutputs(outputs);

    this->vx_graph_->Compile();
    // prepare input
    std::vector<DLTensor*> in_tensors_tvm(this->inputs_.size());
    for (uint32_t i = 0; i < this->inputs_.size(); i++) {
      in_tensors_tvm[i] = args[i];
      inputs[i]->CopyDataToTensor(static_cast<uint8_t*>(in_tensors_tvm[i]->data),
                                  GetDataSize(*(in_tensors_tvm[i])));
    }

    uint64_t tmsStart, tmsEnd, msVal, usVal;

    this->vx_graph_->Run();  // warmup NPU, NPU maybe poweroff
    tmsStart = get_perf_count();
    this->vx_graph_->Run();
    tmsEnd = get_perf_count();
    msVal = (tmsEnd - tmsStart) / 1000000;
    usVal = (tmsEnd - tmsStart) / 1000;
    printf("Process Graph: %ldms or %ldus\n", msVal, usVal);
    // get output
    DLTensor* out_tensor_tvm = args[args.size() - 1];
    outputs[0]->CopyDataFromTensor(static_cast<uint8_t*>(out_tensor_tvm->data));
  });
}

void VsiNpuModule::SerializeTensorSpec(tim::vx::TensorSpec& t_spec, std::ostream& out) {
  std::cout<<"VsiNpuModule : SerializeTensorSpec"<<std::endl;
  TensorSpecIR t_spec_ir;
  t_spec_ir.quant_type = t_spec.quantization_.Type();
  t_spec_ir.channel_dim =  t_spec.quantization_.ChannelDim();
  t_spec_ir.scales = t_spec.quantization_.Scales();
  t_spec_ir.zps = t_spec.quantization_.ZeroPoints();

  t_spec_ir.data_type = t_spec.datatype_;
  t_spec_ir.shape = t_spec.shape_;
  t_spec_ir.attr = t_spec.attr_;

  out.write(reinterpret_cast<const char*>(&t_spec_ir.quant_type), sizeof(t_spec_ir.quant_type));
  out.write(reinterpret_cast<const char*>(&t_spec_ir.channel_dim), sizeof(t_spec_ir.channel_dim));
  uint64_t scales_size = t_spec_ir.scales.size();
  out.write(reinterpret_cast<const char*>(&scales_size), sizeof(scales_size));
  if (scales_size > 0) {
    out.write(reinterpret_cast<const char*>(t_spec_ir.scales.data()), sizeof(t_spec_ir.scales[0]) * scales_size);
    out.write(reinterpret_cast<const char*>(t_spec_ir.zps.data()), sizeof(t_spec_ir.zps[0]) * scales_size);
  }
  out.write(reinterpret_cast<const char*>(&t_spec_ir.data_type), sizeof(t_spec_ir.data_type));
  uint64_t shape_size = t_spec_ir.shape.size();
  out.write(reinterpret_cast<const char*>(&shape_size), sizeof(shape_size));
  if (shape_size > 0) {
    out.write(reinterpret_cast<const char*>(t_spec_ir.shape.data()), sizeof(t_spec_ir.shape[0]) * shape_size);
  }
  out.write(reinterpret_cast<const char*>(&t_spec_ir.attr), sizeof(t_spec_ir.attr));
  std::cout<<"VsiNpuModule : SerializeTensorSpec2"<<std::endl;
}

 tim::vx::TensorSpec VsiNpuModule::DeSerializeTensorSpec(std::istream& in) {
   std::cout<<"VsiNpuModule : DeSerializeTensorSpec"<<std::endl;
   TensorSpecIR t_spec_ir;
   in.read(reinterpret_cast<char*>(&t_spec_ir.quant_type), sizeof(t_spec_ir.quant_type));
   in.read(reinterpret_cast<char*>(&t_spec_ir.channel_dim), sizeof(t_spec_ir.channel_dim));
   uint64_t scales_size = 0;
   in.read(reinterpret_cast<char*>(&scales_size), sizeof(scales_size));
   if (scales_size > 0) {
     t_spec_ir.scales.resize(scales_size);
     t_spec_ir.zps.resize(scales_size);
     in.read(reinterpret_cast<char*>(t_spec_ir.scales.data()), sizeof(t_spec_ir.scales[0]) * scales_size);
     in.read(reinterpret_cast<char*>(t_spec_ir.zps.data()), sizeof(t_spec_ir.zps[0]) * scales_size);
   }
   in.read(reinterpret_cast<char*>(&t_spec_ir.data_type), sizeof(t_spec_ir.data_type));
   uint64_t shape_size = 0;
   in.read(reinterpret_cast<char*>(&shape_size), sizeof(shape_size));
   if (shape_size > 0) {
     t_spec_ir.shape.resize(shape_size);
     in.read(reinterpret_cast<char*>(t_spec_ir.shape.data()), sizeof(t_spec_ir.shape[0]) * shape_size);
   }
   in.read(reinterpret_cast<char*>(&t_spec_ir.attr), sizeof(t_spec_ir.attr));

   tim::vx::Quantization quant(t_spec_ir.quant_type, t_spec_ir.channel_dim, t_spec_ir.scales, t_spec_ir.zps);
   tim::vx::TensorSpec t_spec(t_spec_ir.data_type, t_spec_ir.shape, t_spec_ir.attr, quant);
   std::cout<<"VsiNpuModule : DeSerializeTensorSpec2"<<std::endl;
   return t_spec;
 }

void VsiNpuModule::SaveToBinary(dmlc::Stream* stream) {
  // todo: we need save input/output information to stream
  std::cout << "VsiNpuModule::SaveToBinary" << std::endl;
  stream->Write(this->nbg_size_);
  std::cout << __FUNCTION__ << ": nbg size = " << this->nbg_size_ << std::endl;
  stream->Write(this->compiled_nbg_.get(), this->nbg_size_);

  stream->Write(this->inputs_.size());
  std::cout << __FUNCTION__ << ": input size = " << this->inputs_.size() << std::endl;
  stream->Write(this->outputs_.size());
  std::cout << __FUNCTION__ << ": output size = " << this->outputs_.size() << std::endl;

  for (auto& t_spec : this->inputs_) {
    std::stringstream ss;
    this->SerializeTensorSpec(t_spec, ss);
    stream->Write(ss.str());
  }
  for (auto& t_spec : this->outputs_) {
    std::stringstream ss;
    this->SerializeTensorSpec(t_spec, ss);
    stream->Write(ss.str());
  }
  std::cout << "VsiNpuModule::SaveToBinary2" << std::endl;
}

Module VsiNpuModule::LoadFromBinary(void* strm) {
  // todo: deserialize from stream
  std::cout << "VsiNpuModule::LoadFromBinary" << std::endl;
  auto stream = static_cast<dmlc::Stream*>(strm);
  uint32_t nbg_size;
  stream->Read(&nbg_size);
  std::cout << __FUNCTION__ << ": nbg size = " << nbg_size << std::endl;
  std::shared_ptr<char> nbg_buf(new char [nbg_size]); // TODO: memory leak risk, need double confirm
  stream->Read(nbg_buf.get(), nbg_size);

  uint64_t input_size;
  stream->Read(&input_size);
  std::cout << __FUNCTION__ << ": input size = " << input_size << std::endl;

  uint64_t output_size;
  stream->Read(&output_size);
  std::cout << __FUNCTION__ << ": output size = " << output_size << std::endl;

  std::vector<tim::vx::TensorSpec> inputs_spec;
  for (size_t i = 0; i < input_size; i++) {
    std::string ss;
    stream->Read(&ss);
    std::istringstream t_spec_ss(ss);
    inputs_spec.push_back(DeSerializeTensorSpec(t_spec_ss));
  }

  std::vector<tim::vx::TensorSpec> outputs_spec;
  for (size_t i = 0; i < output_size; i++) {
    std::string ss;
    stream->Read(&ss);
    std::istringstream t_spec_ss(ss);
    outputs_spec.push_back(DeSerializeTensorSpec(t_spec_ss));
  }

  return tvm::runtime::Module(make_object<tvm::runtime::vsi_npu::VsiNpuModule>(
      nbg_buf, nbg_size, inputs_spec, outputs_spec));
}

TVM_REGISTER_GLOBAL("runtime.module.loadbinary_vsi_npu")
    .set_body([](TVMArgs args, TVMRetValue* rv) { *rv = VsiNpuModule::LoadFromBinary(args[0]);});

}  // namespace vsi_npu
}  // namespace runtime
}  // namespace tvm