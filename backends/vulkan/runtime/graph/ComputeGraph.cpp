/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

// @lint-ignore-every CLANGTIDY
// facebook-security-vulnerable-integer-sign-conversion

#include <executorch/backends/vulkan/runtime/graph/ComputeGraph.h>

#include <executorch/backends/vulkan/runtime/graph/ops/impl/Staging.h>

#include <executorch/backends/vulkan/runtime/graph/ops/utils/StagingUtils.h>

namespace vkcompute {

//
// VTensorPtr
//

#define VALUE_PTR_CLASS_IMPL(classname, ctype, type_name)                 \
  classname::classname(ComputeGraph* const graph, const ValueRef idx)     \
      : graph_(graph), ptr_(&(graph_->values_.at(idx).to##type_name())) { \
    graph_->values_in_use_++;                                             \
  }                                                                       \
  ctype* classname::operator->() const {                                  \
    return ptr_;                                                          \
  }                                                                       \
  ctype& classname::operator*() const {                                   \
    return *ptr_;                                                         \
  }                                                                       \
  classname::~classname() {                                               \
    graph_->values_in_use_--;                                             \
  }

VALUE_PTR_CLASS_IMPL(vTensorPtr, api::vTensor, Tensor)
VALUE_PTR_CLASS_IMPL(TensorRefPtr, TensorRef, TensorRef)
VALUE_PTR_CLASS_IMPL(StagingPtr, api::StagingBuffer, Staging)
VALUE_PTR_CLASS_IMPL(IntListPtr, std::vector<int64_t>, IntList)
VALUE_PTR_CLASS_IMPL(DoubleListPtr, std::vector<double>, DoubleList)
VALUE_PTR_CLASS_IMPL(BoolListPtr, std::vector<bool>, BoolList)
VALUE_PTR_CLASS_IMPL(ValueListPtr, std::vector<ValueRef>, ValueList)
VALUE_PTR_CLASS_IMPL(SymIntPtr, SymInt, SymInt)

#undef VALUE_PTR_CLASS_IMPL

//
// TmpTensor
//

TmpTensor::TmpTensor(
    ComputeGraph* const graph_ptr,
    const std::vector<int64_t>& sizes,
    const vkapi::ScalarType dtype,
    const utils::StorageType storage_type,
    const utils::GPUMemoryLayout memory_layout)
    : graph_p(graph_ptr),
      sobj_idx(get_sobj_idx()),
      vref(graph_p->add_tensor(
          sizes,
          dtype,
          storage_type,
          memory_layout,
          sobj_idx)) {}

TmpTensor::TmpTensor(
    ComputeGraph* const graph_ptr,
    const std::vector<int64_t>& sizes,
    const vkapi::ScalarType dtype,
    const utils::StorageType storage_type)
    : graph_p(graph_ptr),
      sobj_idx(get_sobj_idx()),
      vref(graph_p->add_tensor(sizes, dtype, storage_type, sobj_idx)) {}

TmpTensor::TmpTensor(
    ComputeGraph* const graph_ptr,
    const std::vector<int64_t>& sizes,
    const vkapi::ScalarType dtype,
    const utils::GPUMemoryLayout memory_layout)
    : graph_p(graph_ptr),
      sobj_idx(get_sobj_idx()),
      vref(graph_p->add_tensor(sizes, dtype, memory_layout, sobj_idx)) {}

TmpTensor::TmpTensor(
    ComputeGraph* const graph_ptr,
    const std::vector<int64_t>& sizes,
    const vkapi::ScalarType dtype)
    : graph_p(graph_ptr),
      sobj_idx(get_sobj_idx()),
      vref(graph_p->add_tensor(sizes, dtype, sobj_idx)) {}

TmpTensor::~TmpTensor() {
  // Lifetime of this temporary tensor is expired; return the shared object to
  // the pool, as long as the sobj index is valid
  if (sobj_idx >= 0) {
    graph_p->tmp_shared_object_idxs_.emplace(sobj_idx);
  }
}

int64_t TmpTensor::get_sobj_idx() {
  int64_t sobj_idx;
  // If no available temporary shared objects, request a new one to be created
  if (graph_p->tmp_shared_object_idxs_.empty()) {
    sobj_idx = graph_p->shared_objects_.size();
  } else {
    // Get the first available shared object idx
    sobj_idx = graph_p->tmp_shared_object_idxs_.top();
    graph_p->tmp_shared_object_idxs_.pop();
  }
  return sobj_idx;
}

//
// ComputeGraph
//

ComputeGraph::ComputeGraph(GraphConfig config)
    : config_{config},
      prepack_descriptor_counts_{},
      execute_descriptor_counts_{},
      context_{new api::Context(
          config.external_adapter ? config.external_adapter
                                  : vkapi::runtime()->get_adapter_p(),
          config_.context_config)},
      shared_objects_{},
      values_{},
      param_ubos_{},
      prepack_nodes_{},
      execute_nodes_{},
      inputs_{},
      outputs_{} {
  // Ensure that descriptor counts are initialized to 0
  prepack_descriptor_counts_.descriptor_pool_max_sets = 0;
  prepack_descriptor_counts_.descriptor_uniform_buffer_count = 0;
  prepack_descriptor_counts_.descriptor_storage_buffer_count = 0;
  prepack_descriptor_counts_.descriptor_combined_sampler_count = 0;
  prepack_descriptor_counts_.descriptor_storage_image_count = 0;

  execute_descriptor_counts_.descriptor_pool_max_sets = 0;
  execute_descriptor_counts_.descriptor_uniform_buffer_count = 0;
  execute_descriptor_counts_.descriptor_storage_buffer_count = 0;
  execute_descriptor_counts_.descriptor_combined_sampler_count = 0;
  execute_descriptor_counts_.descriptor_storage_image_count = 0;

  // If certain graph config variables are not specified, then set them
  // automatically.
  if (config_.prepack_threshold_nbytes == 0) {
    config_.prepack_threshold_nbytes = 10 * MB;
    config_.prepack_initial_threshold_nbytes = 10 * MB;
  }
}

ComputeGraph::~ComputeGraph() {
  values_.clear();

  prepack_nodes_.clear();
  execute_nodes_.clear();
  clear_deferred_cmds();

  context_->flush();
}

std::vector<int64_t> ComputeGraph::extract_int_or_symint_list(
    const ValueRef idx) {
  const Value& val = values_.at(idx);
  std::vector<int64_t> result;

  if (val.isIntList()) {
    // If it's an IntList, return a copy of the list
    return val.toConstIntList();
  } else if (val.isValueList()) {
    // If it's a ValueList, extract each element as an Int or SymInt
    const std::vector<ValueRef>& value_list = val.toConstValueList();
    result.reserve(value_list.size());

    for (const ValueRef& ref : value_list) {
      const Value& element = values_.at(ref);
      if (element.isInt()) {
        result.push_back(element.toInt());
      } else if (element.isSymInt()) {
        result.push_back(read_symint(ref));
      } else {
        VK_THROW(
            "ValueList element is neither Int nor SymInt, but has type ",
            element.type());
      }
    }
    return result;
  }

  VK_THROW(
      "Cannot extract int or symint list from Value with type ", val.type());
}

utils::StorageType ComputeGraph::suggested_storage_type() {
  if (config_.enable_storage_type_override) {
    return config_.storage_type_override;
  }
  return utils::kTexture3D;
}

utils::GPUMemoryLayout ComputeGraph::suggested_memory_layout(
    const std::vector<int64_t>& sizes) {
  if (config_.enable_memory_layout_override) {
    return config_.memory_layout_override;
  }
  if (sizes.size() < 3) {
    return utils::kWidthPacked;
  }
  // For 3 dimensional tensors that only have a channels dimension of 1, still
  // prefer width packed.
  if (utils::val_at(-3, sizes) == 1) {
    return utils::kWidthPacked;
  }
  return utils::kChannelsPacked;
}

bool ComputeGraph::device_name_contains(const char* substr) {
  return context_->adapter_ptr()->device_name().find(substr) !=
      std::string::npos;
}

void ComputeGraph::check_no_active_value_ptrs() {
  VK_CHECK_COND(
      values_in_use_ == 0,
      "Make sure that there are no pointers stored from the return values of "
      "`ComputeGraph::get_*()` functions in scope before adding Values to the "
      "graph. Modifying the graph's values may cause existing pointers to be "
      "invalidated.");
}

std::vector<int64_t> ComputeGraph::sizes_of(const ValueRef idx) const {
  const Value& val = values_.at(idx);
  if (val.isTensor()) {
    return val.toConstTensor().sizes();
  } else if (val.isTensorRef()) {
    return val.toConstTensorRef().sizes;
  }
  VK_THROW("Could not get sizes of value with type ", val.type());
}

int64_t ComputeGraph::dim_of(const ValueRef idx) const {
  const Value& val = values_.at(idx);
  if (val.isTensor()) {
    return val.toConstTensor().dim();
  } else if (val.isTensorRef()) {
    return val.toConstTensorRef().sizes.size();
  }
  VK_THROW("Could not get dim of value with type ", val.type());
}

std::vector<int64_t> ComputeGraph::dim_order_of(const ValueRef idx) const {
  const Value& val = values_.at(idx);
  if (val.isTensor()) {
    return val.toConstTensor().dim_order();
  }
  VK_THROW("Could not get dim order of value with type ", val.type());
}

std::vector<int64_t> ComputeGraph::strides_of(const ValueRef idx) const {
  const Value& val = values_.at(idx);
  if (val.isTensor()) {
    return val.toConstTensor().strides();
  }
  VK_THROW("Could not get strides of value with type ", val.type());
}

vkapi::ScalarType ComputeGraph::dtype_of(const ValueRef idx) const {
  const Value& val = values_.at(idx);
  if (val.isTensor()) {
    return val.toConstTensor().dtype();
  } else if (val.isTensorRef()) {
    return val.toConstTensorRef().dtype;
  } else if (val.isBool()) {
    return vkapi::ScalarType::Bool;
  } else if (val.isDouble()) {
    // We downcast anyway in the shader and we want to avoid having to
    // write special cases there.
    return vkapi::ScalarType::Float;
  } else if (val.isInt()) {
    return vkapi::ScalarType::Int;
  }
  VK_THROW("Could not get dtype of value with type ", val.type());
}

bool ComputeGraph::is_contiguous_buffer_tensor(const ValueRef idx) const {
  if (!val_is_tensor(idx)) {
    return false;
  }
  if (!is_buffer_storage(idx)) {
    return false;
  }
  return is_contiguous(idx);
}

bool ComputeGraph::is_standard_channels_packed_texture_tensor(
    const ValueRef idx) const {
  if (!val_is_tensor(idx)) {
    return false;
  }
  if (is_buffer_storage(idx)) {
    return false;
  }
  return has_standard_axis_map(idx) && packed_dim_of(idx) == 2;
}

bool ComputeGraph::is_standard_width_packed_texture_tensor(
    const ValueRef idx) const {
  if (!val_is_tensor(idx)) {
    return false;
  }
  if (is_buffer_storage(idx)) {
    return false;
  }
  return has_standard_axis_map(idx) && packed_dim_of(idx) == 0;
}

ValueRef ComputeGraph::add_tensor(
    const std::vector<int64_t>& sizes,
    const vkapi::ScalarType dtype,
    const utils::StorageType storage_type,
    const utils::GPUMemoryLayout memory_layout,
    const int64_t shared_object_idx,
    const utils::AxisMapLayout axis_map_layout) {
  bool allocate_memory = shared_object_idx < 0;

  ValueRef idx(static_cast<int>(values_.size()));
  check_no_active_value_ptrs();
  values_.emplace_back(api::vTensor(
      context(),
      sizes,
      dtype,
      storage_type,
      memory_layout,
      allocate_memory,
      axis_map_layout));

  if (!allocate_memory) {
    get_shared_object(shared_object_idx).add_user(this, idx);
  }
  return idx;
}

ValueRef ComputeGraph::add_tensor(
    const std::vector<int64_t>& sizes,
    const vkapi::ScalarType dtype,
    const utils::StorageType storage_type,
    const int64_t shared_object_idx,
    const utils::AxisMapLayout axis_map_layout) {
  return add_tensor(
      sizes,
      dtype,
      storage_type,
      suggested_memory_layout(sizes),
      shared_object_idx,
      axis_map_layout);
}

ValueRef ComputeGraph::add_tensor(
    const std::vector<int64_t>& sizes,
    const vkapi::ScalarType dtype,
    const utils::GPUMemoryLayout memory_layout,
    const int64_t shared_object_idx,
    const utils::AxisMapLayout axis_map_layout) {
  return add_tensor(
      sizes,
      dtype,
      suggested_storage_type(),
      memory_layout,
      shared_object_idx,
      axis_map_layout);
}

ValueRef ComputeGraph::add_tensor_like(
    const ValueRef idx,
    const utils::StorageType storage_type,
    const utils::GPUMemoryLayout memory_layout,
    const utils::AxisMapLayout axis_map_layout) {
  return add_tensor(
      sizes_of(idx),
      dtype_of(idx),
      storage_type,
      memory_layout,
      -1,
      axis_map_layout);
}

ValueRef ComputeGraph::add_tensor_like(
    const ValueRef idx,
    const utils::GPUMemoryLayout memory_layout,
    const utils::AxisMapLayout axis_map_layout) {
  return add_tensor(
      sizes_of(idx),
      dtype_of(idx),
      storage_type_of(idx),
      memory_layout,
      -1,
      axis_map_layout);
}

ValueRef ComputeGraph::add_tensor(
    const std::vector<int64_t>& sizes,
    const vkapi::ScalarType dtype,
    const int64_t shared_object_idx,
    const utils::AxisMapLayout axis_map_layout) {
  return add_tensor(
      sizes,
      dtype,
      suggested_memory_layout(sizes),
      shared_object_idx,
      axis_map_layout);
}

ValueRef ComputeGraph::add_tensor(const vkapi::VulkanImage& image) {
  ValueRef idx(static_cast<int>(values_.size()));
  check_no_active_value_ptrs();
  values_.emplace_back(api::vTensor(context(), image));
  return idx;
}

ValueRef ComputeGraph::add_tensor_view(const ValueRef vref) {
  const vTensorPtr t = get_tensor(vref);
  ValueRef idx(static_cast<int>(values_.size()));
  values_.emplace_back(api::vTensor(*t));
  return idx;
}

ValueRef ComputeGraph::add_tensor_view(
    const ValueRef vref,
    const std::vector<int64_t>& sizes,
    const std::vector<int64_t>& strides) {
  const vTensorPtr t = get_tensor(vref);
  ValueRef idx(static_cast<int>(values_.size()));
  values_.emplace_back(api::vTensor(*t, sizes, strides));
  return idx;
}

ValueRef ComputeGraph::add_tensorref(
    const std::vector<int64_t>& sizes,
    const vkapi::ScalarType dtype,
    const void* const data) {
  ValueRef idx(static_cast<int>(values_.size()));
  check_no_active_value_ptrs();
  values_.emplace_back(TensorRef(sizes, dtype, data));
  total_constant_nbytes_ += values_.back().toConstTensorRef().nbytes();
  return idx;
}

ValueRef ComputeGraph::add_staging(
    const vkapi::ScalarType dtype,
    const size_t numel) {
  ValueRef idx(static_cast<int>(values_.size()));
  check_no_active_value_ptrs();
  values_.emplace_back(api::StagingBuffer(context(), dtype, numel));
  return idx;
}

ValueRef ComputeGraph::add_none() {
  ValueRef idx(static_cast<int>(values_.size()));
  check_no_active_value_ptrs();
  values_.emplace_back();
  return idx;
}

ValueRef ComputeGraph::add_value_list(std::vector<ValueRef>&& value) {
  ValueRef idx(static_cast<int>(values_.size()));
  check_no_active_value_ptrs();
  values_.emplace_back(std::move(value));
  return idx;
}

ValueRef ComputeGraph::add_string(std::string&& str) {
  ValueRef idx(static_cast<int>(values_.size()));
  check_no_active_value_ptrs();
  values_.emplace_back(std::move(str));
  return idx;
}

ValueRef ComputeGraph::add_symint(const int32_t val) {
  ValueRef idx(static_cast<int>(values_.size()));
  check_no_active_value_ptrs();
  values_.emplace_back(SymInt(context(), val));
  return idx;
}

ValueRef ComputeGraph::get_or_add_value_for_int(const int64_t val) {
  for (int i = 0; i < values_.size(); ++i) {
    if (values_.at(i).isInt() && values_.at(i).toInt() == val) {
      return i;
    }
  }
  return add_scalar(val);
}

ValueRef ComputeGraph::set_input_tensor(
    const ValueRef idx,
    const bool use_staging) {
  if (use_staging) {
    vkapi::ScalarType dtype = get_tensor(idx)->dtype();
    // For texture storage, the buffer size needs to account for the zero
    // padding applied by unused texel elements.
    size_t buf_numel = get_tensor(idx)->staging_buffer_numel();
    ValueRef staging_idx = add_staging(dtype, buf_numel);
    add_staging_to_tensor_node(*this, staging_idx, idx);
    inputs_.push_back({idx, staging_idx});
    return staging_idx;
  }
  inputs_.push_back({idx, kDummyValueRef});
  return idx;
}

ValueRef ComputeGraph::set_output_tensor(
    const ValueRef idx,
    const bool use_staging) {
  if (use_staging) {
    vkapi::ScalarType dtype = get_tensor(idx)->dtype();
    // For texture storage, the buffer size needs to account for the zero
    // padding applied by unused texel elements.
    size_t buf_numel = get_tensor(idx)->staging_buffer_numel();
    ValueRef staging_idx = add_staging(dtype, buf_numel);
    // We only run this when the tensor is non-empty.  When the underlying
    // tensor is empty (e.g. padded_numel == 0), we do not allocate a VkImage to
    // tensor, we will not be able to bind the node for execution.
    if (buf_numel > 0) {
      add_tensor_to_staging_node(*this, idx, staging_idx);
    }
    outputs_.push_back({idx, staging_idx});
    return staging_idx;
  }
  outputs_.push_back({idx, kDummyValueRef});
  return idx;
}

ValueRef ComputeGraph::set_output_value(const ValueRef idx) {
  if (values_.at(idx).isTensor()) {
    return set_output_tensor(idx);
  }
  outputs_.push_back({idx, kDummyValueRef});
  return idx;
}

vkapi::BufferBindInfo ComputeGraph::get_or_create_int_param_buffer(
    const ValueRef idx) {
  if (values_.at(idx).isInt()) {
    const int32_t val = extract_scalar<int32_t>(idx);
    return create_params_buffer(val);
  } else if (values_.at(idx).isSymInt()) {
    SymIntPtr symint = get_symint(idx);
    return vkapi::BufferBindInfo(symint->gpu_buffer.buffer());
  }
  VK_THROW("Cannot create a int param buffer for the given value");
}

vkapi::BufferBindInfo ComputeGraph::get_or_create_int_param_buffer(
    const ValueRef idx,
    const int32_t default_val) {
  if (values_.at(idx).isNone()) {
    return create_params_buffer(default_val);
  } else {
    return get_or_create_int_param_buffer(idx);
  }
}

void ComputeGraph::set_symint(const ValueRef idx, const int32_t val) {
  get_symint(idx)->set(val);
}

int32_t ComputeGraph::read_symint(const ValueRef idx) {
  return get_symint(idx)->get();
}

SharedObject& ComputeGraph::get_shared_object(const int64_t idx) {
  if (idx >= shared_objects_.size()) {
    shared_objects_.resize(static_cast<size_t>(idx + 1));
  }
  return shared_objects_.at(idx);
}

void ComputeGraph::update_descriptor_counts(
    const vkapi::ShaderInfo& shader_info,
    bool execute) {
  vkapi::DescriptorPoolConfig* config =
      execute ? &execute_descriptor_counts_ : &prepack_descriptor_counts_;

  config->descriptor_pool_max_sets += 1;
  for (const VkDescriptorType arg_type : shader_info.kernel_layout) {
    switch (arg_type) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        config->descriptor_uniform_buffer_count += 1;
        break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        config->descriptor_storage_buffer_count += 1;
        break;
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        config->descriptor_combined_sampler_count += 1;
        break;
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        config->descriptor_storage_image_count += 1;
        break;
      default:
        VK_THROW("Unsupported descriptor type!");
    }
  }
}

void ComputeGraph::register_pipeline_to_create(
    const vkapi::ShaderInfo& shader_info,
    const utils::WorkgroupSize& local_workgroup_size,
    const vkapi::SpecVarList& spec_vars,
    const std::vector<PushConstantDataInfo>& push_constants) {
  VkDescriptorSetLayout shader_layout =
      context()->shader_layout_cache().retrieve(shader_info.kernel_layout);

  uint32_t pc_offset = 0;
  std::array<uint8_t, kMaxPushConstantSize> pc_data;
  for (const auto& pc : push_constants) {
    pc_offset += pc.write(pc_data.data(), pc_offset, kMaxPushConstantSize);
  }

  vkapi::SpecVarList spec_constants = {
      SV(local_workgroup_size[0u]),
      SV(local_workgroup_size[1u]),
      SV(local_workgroup_size[2u])};

  spec_constants.append(spec_vars);

  const vkapi::ComputePipelineCache::Key desc = {
      context()->pipeline_layout_cache().retrieve(shader_layout, pc_offset),
      context()->shader_cache().retrieve(shader_info),
      spec_constants};

  if (context_->pipeline_cache().contains(desc)) {
    return;
  }
  auto it = pipeline_descriptors_.find(desc);
  if (it != pipeline_descriptors_.cend()) {
    return;
  }
  pipeline_descriptors_.insert(desc);
}

utils::uvec3 ComputeGraph::create_global_wg_size(const ValueRef idx) {
  if (is_buffer_storage(idx)) {
    return {uint32_t(numel_of(idx)), 1u, 1u};
  }
  return logical_limits_of(idx);
}

utils::uvec3 ComputeGraph::create_local_wg_size(
    const utils::uvec3 global_wg_size) {
  if (config_.enable_local_wg_size_override) {
    return config_.local_wg_size_override;
  }

  // array containing axis index and global workgroup size
  std::pair<uint32_t, uint32_t> global_wg_size_desc[] = {
      {0u, global_wg_size[0]},
      {1u, global_wg_size[1]},
      {2u, global_wg_size[2]}};

  // sort the global workgroup size in descending order
  if (global_wg_size_desc[0].second < global_wg_size_desc[1].second) {
    std::swap(global_wg_size_desc[0], global_wg_size_desc[1]);
  }
  if (global_wg_size_desc[1].second < global_wg_size_desc[2].second) {
    std::swap(global_wg_size_desc[1], global_wg_size_desc[2]);
  }
  if (global_wg_size_desc[0].second < global_wg_size_desc[1].second) {
    std::swap(global_wg_size_desc[0], global_wg_size_desc[1]);
  }

  utils::uvec3 local_group_size = {
      8,
      std::max(1u, std::min(4u, global_wg_size_desc[1].second)),
      std::max(1u, std::min(2u, global_wg_size_desc[2].second))};

  if (global_wg_size_desc[2u].second == 1) {
    if (global_wg_size_desc[1u].second == 1) {
      local_group_size[0u] = 64;
      local_group_size[1u] = 1;
    } else if (global_wg_size_desc[1u].second % 4 == 0) {
      local_group_size[0u] = 16;
      local_group_size[1u] = 4;
    } else {
      local_group_size[0u] = 32;
      local_group_size[1u] = 2;
    }
  }

  return {
      local_group_size[global_wg_size_desc[0].first],
      local_group_size[global_wg_size_desc[1].first],
      local_group_size[global_wg_size_desc[2].first]};
}

utils::uvec3 ComputeGraph::create_local_wg_size(const ValueRef idx) {
  return create_local_wg_size(create_global_wg_size(idx));
}

void ComputeGraph::copy_into_staging(
    const ValueRef idx,
    const void* data,
    const size_t numel) {
  StagingPtr staging = get_staging(idx);
  size_t nbytes = numel * vkapi::element_size(staging->dtype());
  staging->copy_from(data, nbytes);
}

void ComputeGraph::copy_from_staging(
    const ValueRef idx,
    void* data,
    const size_t numel) {
  StagingPtr staging = get_staging(idx);
  size_t nbytes = numel * vkapi::element_size(staging->dtype());
  staging->copy_to(data, nbytes);
}

void ComputeGraph::prepare() {
#define MERGE_FIELD(field)                    \
  static_cast<uint32_t>(std::ceil(            \
      std::max(                               \
          execute_descriptor_counts_.field,   \
          prepack_descriptor_counts_.field) * \
      config_.descriptor_pool_safety_factor))

  uint32_t max_sets = MERGE_FIELD(descriptor_pool_max_sets);
  vkapi::DescriptorPoolConfig config{
      max_sets,
      std::max(MERGE_FIELD(descriptor_uniform_buffer_count), max_sets),
      std::max(MERGE_FIELD(descriptor_storage_buffer_count), max_sets),
      std::max(MERGE_FIELD(descriptor_combined_sampler_count), max_sets),
      std::max(MERGE_FIELD(descriptor_storage_image_count), max_sets),
      1u,
  };

  if (!context_->descriptor_pool()) {
    context_->descriptor_pool().init(config);
  }
#undef MERGE_FIELD

  if (config_.enable_querypool) {
    context_->initialize_querypool();
  }

  for (SharedObject& shared_object : shared_objects_) {
    shared_object.allocate(this);
    shared_object.bind_users(this);
  }
}

void ComputeGraph::prepare_pipelines() {
  for (std::unique_ptr<PrepackNode>& node : prepack_nodes_) {
    node->prepare_pipelines(this);
  }
  for (std::unique_ptr<ExecuteNode>& node : execute_nodes_) {
    node->prepare_pipelines(this);
  }
  context_->pipeline_cache().create_pipelines(pipeline_descriptors_);

  pipeline_descriptors_ = std::unordered_set<
      vkapi::ComputePipelineCache::Key,
      vkapi::ComputePipelineCache::Hasher>();
}

void ComputeGraph::submit_current_cmd(const bool final_use) {
  context_->submit_cmd_to_gpu(VK_NULL_HANDLE, final_use);
}

void ComputeGraph::submit_current_cmd_and_wait(const bool final_use) {
  vkapi::VulkanFence fence = context_->fences().get_fence();
  context_->submit_cmd_to_gpu(fence.get_submit_handle(), final_use);
  fence.wait();
  context_->fences().return_fence(fence);
}

void ComputeGraph::submit_cmd(
    vkapi::CommandBuffer& cmd_buf,
    VkSemaphore wait_semaphore,
    VkSemaphore signal_semaphore,
    VkFence fence) {
  if (cmd_buf) {
    cmd_buf.end();
    context_->adapter_ptr()->submit_cmd(
        context_->queue(),
        cmd_buf.get_submit_handle(false),
        fence,
        wait_semaphore,
        signal_semaphore);
  }
}

void ComputeGraph::submit_deferred_cmds_and_wait() {
  VkSemaphore prev_semaphore = VK_NULL_HANDLE;
  vkapi::VulkanFence fence = context_->fences().get_fence();

  for (uint32_t i = 0; i < deferred_cmd_list_.size(); i++) {
    auto& cmd = deferred_cmd_list_[i];
    VkSemaphore wait_semaphore = prev_semaphore;
    VkSemaphore signal_semaphore = cmd.get_signal_semaphore();
    prev_semaphore = signal_semaphore;

    submit_cmd(
        cmd,
        wait_semaphore,
        signal_semaphore,
        i == (deferred_cmd_list_.size() - 1) ? fence.get_submit_handle()
                                             : VK_NULL_HANDLE);
  }
  fence.wait();
  context_->fences().return_fence(fence);
}

void ComputeGraph::clear_deferred_cmds() {
  for (auto& cmd : deferred_cmd_list_) {
    if (cmd) {
      cmd.end();
      cmd.invalidate();
    }
  }
  deferred_cmd_list_.clear();
}

void ComputeGraph::prepack() {
  int i = 0;
  bool submitted = false;
  const bool reduce_peak_memory = total_constant_nbytes_ > 500 * MB;
  // int count = 0;
  context_->set_cmd();
  for (std::unique_ptr<PrepackNode>& node : prepack_nodes_) {
    // Do not trigger on the first or last prepack node.
    const bool not_terminal = i != 0 && i != (prepack_nodes_.size() - 1);
    size_t threshold = submitted ? config_.prepack_threshold_nbytes
                                 : config_.prepack_initial_threshold_nbytes;
    if (not_terminal && staging_nbytes_in_cmd_ > threshold) {
      // If reducing peak memory usage, wait for the current command buffer to
      // finish executing and flush to recycle the staging memory. This will
      // reduce peak memory usage, but will slightly increase load latency.
      // Otherwise, just submit the current command buffer for execution and
      // proceed. This results in lower load latency at the cost of higher peak
      // memory usage.
      if (reduce_peak_memory) {
        submit_current_cmd_and_wait();
        context_->flush();
      } else {
        submit_current_cmd();
      }
      staging_nbytes_in_cmd_ = 0;
      context_->set_cmd();
      submitted = true;
    }

    node->encode(this);
    i++;
  }
  submit_current_cmd_and_wait(/*final_use=*/true);
  context_->flush();
  staging_nbytes_in_cmd_ = 0;
}

void ComputeGraph::encode_execute() {
  clear_deferred_cmds();
  context_->flush();
  context_->set_cmd(/*reusable = */ true);

  context_->cmd_reset_querypool();

  for (std::unique_ptr<ExecuteNode>& node : execute_nodes_) {
    node->encode(this);
  }

  deferred_cmd_list_.emplace_back(std::move(context_->extract_cmd()));
}

void ComputeGraph::execute() {
  submit_deferred_cmds_and_wait();
  execute_count_++;
}

void ComputeGraph::resize_input(
    const int64_t idx,
    const std::vector<int64_t>& new_sizes) {
  IOValueRef io_val = inputs_.at(idx);
  get_tensor(io_val.value)->virtual_resize(new_sizes);
}

void ComputeGraph::virtual_resize(
    const ValueRef idx,
    const std::vector<int64_t>& new_sizes) {
  get_tensor(idx)->virtual_resize(new_sizes);
}

void ComputeGraph::propagate_resize() {
  for (std::unique_ptr<ExecuteNode>& node : execute_nodes_) {
    node->trigger_resize(this);
  }
  // Only re-encode on resize if dynamic shapes are expected
  if (config_.expect_dynamic_shapes) {
    encode_execute();
  }
}

} // namespace vkcompute
