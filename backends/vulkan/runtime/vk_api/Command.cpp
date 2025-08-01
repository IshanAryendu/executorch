/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <executorch/backends/vulkan/runtime/vk_api/Adapter.h>
#include <executorch/backends/vulkan/runtime/vk_api/Command.h>

#include <mutex>

namespace vkcompute {
namespace vkapi {

//
// CommandBuffer
//

CommandBuffer::CommandBuffer(
    VkCommandBuffer handle,
    VkSemaphore semaphore,
    const VkCommandBufferUsageFlags flags)
    : handle_(handle),
      signal_semaphore_(semaphore),
      flags_(flags),
      state_(CommandBuffer::State::NEW),
      bound_{} {}

CommandBuffer::CommandBuffer(CommandBuffer&& other) noexcept
    : handle_(other.handle_),
      signal_semaphore_(other.signal_semaphore_),
      flags_(other.flags_),
      state_(other.state_),
      bound_(other.bound_) {
  other.handle_ = VK_NULL_HANDLE;
  other.signal_semaphore_ = VK_NULL_HANDLE;
  other.bound_.reset();
}

CommandBuffer& CommandBuffer::operator=(CommandBuffer&& other) noexcept {
  handle_ = other.handle_;
  signal_semaphore_ = other.signal_semaphore_;
  flags_ = other.flags_;
  state_ = other.state_;
  bound_ = other.bound_;

  other.handle_ = VK_NULL_HANDLE;
  other.signal_semaphore_ = VK_NULL_HANDLE;
  other.bound_.reset();
  other.state_ = CommandBuffer::State::INVALID;

  return *this;
}

void CommandBuffer::begin() {
  VK_CHECK_COND(
      state_ == CommandBuffer::State::NEW,
      "Vulkan CommandBuffer: called begin() on a command buffer whose state "
      "is not NEW.");

  const VkCommandBufferBeginInfo begin_info{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      nullptr,
      flags_,
      nullptr,
  };

  VK_CHECK(vkBeginCommandBuffer(handle_, &begin_info));
  state_ = CommandBuffer::State::RECORDING;
}

void CommandBuffer::end() {
  VK_CHECK_COND(
      state_ == CommandBuffer::State::RECORDING ||
          state_ == CommandBuffer::State::SUBMITTED,
      "Vulkan CommandBuffer: called end() on a command buffer whose state "
      "is not RECORDING or SUBMITTED.");

  if (state_ == CommandBuffer::State::RECORDING) {
    VK_CHECK(vkEndCommandBuffer(handle_));
  }
  state_ = CommandBuffer::State::READY;
}

void CommandBuffer::bind_pipeline(
    VkPipeline pipeline,
    VkPipelineLayout pipeline_layout,
    const utils::WorkgroupSize local_workgroup_size) {
  VK_CHECK_COND(
      state_ == CommandBuffer::State::RECORDING,
      "Vulkan CommandBuffer: called bind_pipeline() on a command buffer whose state "
      "is not RECORDING.");

  if (pipeline != bound_.pipeline) {
    vkCmdBindPipeline(handle_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

    bound_.pipeline = pipeline;
  }

  bound_.pipeline_layout = pipeline_layout;
  bound_.local_workgroup_size = local_workgroup_size;

  state_ = CommandBuffer::State::PIPELINE_BOUND;
}

void CommandBuffer::bind_descriptors(VkDescriptorSet descriptors) {
  VK_CHECK_COND(
      state_ == CommandBuffer::State::PIPELINE_BOUND,
      "Vulkan CommandBuffer: called bind_descriptors() on a command buffer whose state "
      "is not PIPELINE_BOUND.");

  if (descriptors != bound_.descriptors) {
    vkCmdBindDescriptorSets(
        handle_, // commandBuffer
        VK_PIPELINE_BIND_POINT_COMPUTE, // pipelineBindPoint
        bound_.pipeline_layout, // layout
        0u, // firstSet
        1u, // descriptorSetCount
        &descriptors, // pDescriptorSets
        0u, // dynamicOffsetCount
        nullptr); // pDynamicOffsets
  }

  bound_.descriptors = descriptors;

  state_ = CommandBuffer::State::DESCRIPTORS_BOUND;
}

void CommandBuffer::set_push_constants(
    VkPipelineLayout pipeline_layout,
    const void* push_constants_data,
    uint32_t push_constants_size) {
  if (push_constants_data != nullptr && push_constants_size > 0) {
    vkCmdPushConstants(
        handle_,
        pipeline_layout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        push_constants_size,
        push_constants_data);
  }
}

void CommandBuffer::insert_barrier(PipelineBarrier& pipeline_barrier) {
  VK_CHECK_COND(
      state_ == CommandBuffer::State::DESCRIPTORS_BOUND ||
          state_ == CommandBuffer::State::RECORDING,
      "Vulkan CommandBuffer: called insert_barrier() on a command buffer whose state "
      "is not DESCRIPTORS_BOUND or RECORDING.");

  if (pipeline_barrier) {
    if (!pipeline_barrier.buffer_barrier_handles.empty()) {
      pipeline_barrier.buffer_barrier_handles.clear();
    }
    for (const BufferMemoryBarrier& memory_barrier : pipeline_barrier.buffers) {
      pipeline_barrier.buffer_barrier_handles.push_back(memory_barrier.handle);
    }

    if (!pipeline_barrier.image_barrier_handles.empty()) {
      pipeline_barrier.image_barrier_handles.clear();
    }
    for (const ImageMemoryBarrier& memory_barrier : pipeline_barrier.images) {
      pipeline_barrier.image_barrier_handles.push_back(memory_barrier.handle);
    }
    vkCmdPipelineBarrier(
        handle_, // commandBuffer
        pipeline_barrier.stage.src, // srcStageMask
        pipeline_barrier.stage.dst, // dstStageMask
        0u, // dependencyFlags
        0u, // memoryBarrierCount
        nullptr, // pMemoryBarriers
        pipeline_barrier.buffers.size(), // bufferMemoryBarrierCount
        !pipeline_barrier.buffers.empty()
            ? pipeline_barrier.buffer_barrier_handles.data()
            : nullptr, // pMemoryBarriers
        pipeline_barrier.images.size(), // imageMemoryBarrierCount
        !pipeline_barrier.images.empty()
            ? pipeline_barrier.image_barrier_handles.data()
            : nullptr); // pImageMemoryBarriers
  }

  state_ = CommandBuffer::State::BARRIERS_INSERTED;
}

void CommandBuffer::dispatch(const utils::uvec3& global_workgroup_size) {
  VK_CHECK_COND(
      state_ == CommandBuffer::State::BARRIERS_INSERTED,
      "Vulkan CommandBuffer: called dispatch() on a command buffer whose state "
      "is not BARRIERS_INSERTED.");

  vkCmdDispatch(
      handle_,
      utils::div_up(global_workgroup_size[0u], bound_.local_workgroup_size[0u]),
      utils::div_up(global_workgroup_size[1u], bound_.local_workgroup_size[1u]),
      utils::div_up(
          global_workgroup_size[2u], bound_.local_workgroup_size[2u]));

  state_ = CommandBuffer::State::RECORDING;
}

void CommandBuffer::blit(vkapi::VulkanImage& src, vkapi::VulkanImage& dst) {
  VK_CHECK_COND(
      state_ == CommandBuffer::State::BARRIERS_INSERTED,
      "Vulkan CommandBuffer: called blit() on a command buffer whose state "
      "is not BARRIERS_INSERTED.");

  auto src_extents = src.extents();
  auto dst_extents = dst.extents();

  VkImageBlit blit{};
  blit.srcOffsets[0] = {0, 0, 0},
  blit.srcOffsets[1] =
      {static_cast<int32_t>(src_extents.width),
       static_cast<int32_t>(src_extents.height),
       static_cast<int32_t>(src_extents.depth)},
  blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
  blit.srcSubresource.mipLevel = 0, blit.srcSubresource.baseArrayLayer = 0,
  blit.srcSubresource.layerCount = 1, blit.dstOffsets[0] = {0, 0, 0},
  blit.dstOffsets[1] =
      {static_cast<int32_t>(dst_extents.width),
       static_cast<int32_t>(dst_extents.height),
       static_cast<int32_t>(dst_extents.depth)},
  blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
  blit.dstSubresource.mipLevel = 0, blit.dstSubresource.baseArrayLayer = 0,
  blit.dstSubresource.layerCount = 1,

  vkCmdBlitImage(
      handle_,
      src.handle(),
      src.layout(),
      dst.handle(),
      dst.layout(),
      1,
      &blit,
      VK_FILTER_NEAREST);

  state_ = CommandBuffer::State::RECORDING;
}

void CommandBuffer::write_timestamp(VkQueryPool querypool, const uint32_t idx)
    const {
  VK_CHECK_COND(
      state_ == CommandBuffer::State::RECORDING,
      "Vulkan CommandBuffer: called write_timestamp() on a command buffer whose state "
      "is not RECORDING.");

  vkCmdWriteTimestamp(
      handle_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, querypool, idx);
}

void CommandBuffer::reset_querypool(
    VkQueryPool querypool,
    const uint32_t first_idx,
    const uint32_t count) const {
  VK_CHECK_COND(
      state_ == CommandBuffer::State::RECORDING,
      "Vulkan CommandBuffer: called reset_querypool() on a command buffer whose state "
      "is not RECORDING.");

  vkCmdResetQueryPool(handle_, querypool, first_idx, count);
}

VkCommandBuffer CommandBuffer::get_submit_handle(const bool final_use) {
  VK_CHECK_COND(
      state_ == CommandBuffer::State::READY,
      "Vulkan CommandBuffer: called begin() on a command buffer whose state "
      "is not READY.");

  VkCommandBuffer handle = handle_;

  if (!is_reusable() || final_use) {
    invalidate();
  }
  state_ = CommandBuffer::State::SUBMITTED;

  return handle;
}

//
// CommandPool
//

CommandPool::CommandPool(
    VkDevice device,
    const uint32_t queue_family_idx,
    const CommandPoolConfig& config)
    : device_(device),
      queue_family_idx_(queue_family_idx),
      pool_(VK_NULL_HANDLE),
      config_(config),
      mutex_{},
      buffers_{},
      in_use_(0u) {
  const VkCommandPoolCreateInfo create_info{
      VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      nullptr,
      VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      queue_family_idx_,
  };

  VK_CHECK(vkCreateCommandPool(device_, &create_info, nullptr, &pool_));

  // Pre-allocate some command buffers
  allocate_new_batch(config_.cmd_pool_initial_size);
}

CommandPool::~CommandPool() {
  if (pool_ == VK_NULL_HANDLE) {
    return;
  }
  for (auto& semaphore : semaphores_) {
    if (semaphore != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, semaphore, nullptr);
    }
  }

  vkDestroyCommandPool(device_, pool_, nullptr);
}

CommandBuffer CommandPool::get_new_cmd(bool reusable) {
  std::lock_guard<std::mutex> lock(mutex_);

  // No-ops if there are command buffers available
  allocate_new_batch(config_.cmd_pool_batch_size);

  VkCommandBuffer handle = buffers_[in_use_];
  VkSemaphore semaphore = semaphores_[in_use_];

  VkCommandBufferUsageFlags cmd_flags = 0u;
  if (!reusable) {
    cmd_flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  }

  in_use_++;
  return CommandBuffer(handle, semaphore, cmd_flags);
}

void CommandPool::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  VK_CHECK(vkResetCommandPool(device_, pool_, 0u));
  in_use_ = 0u;
}

void CommandPool::allocate_new_batch(const uint32_t count) {
  // No-ops if there are still command buffers available
  if (in_use_ < buffers_.size()) {
    return;
  }

  buffers_.resize(buffers_.size() + count);
  semaphores_.resize(buffers_.size() + count);

  const VkCommandBufferAllocateInfo allocate_info{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, // sType
      nullptr, // pNext
      pool_, // commandPool
      VK_COMMAND_BUFFER_LEVEL_PRIMARY, // level
      count, // commandBufferCount
  };

  VK_CHECK(vkAllocateCommandBuffers(
      device_, &allocate_info, buffers_.data() + in_use_));

  const VkSemaphoreCreateInfo semaphoreCreateInfo = {
      VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};

  for (uint32_t i = 0; i < count; i++) {
    VK_CHECK(vkCreateSemaphore(
        device_,
        &semaphoreCreateInfo,
        nullptr,
        semaphores_.data() + in_use_ + i));
  }
}

} // namespace vkapi
} // namespace vkcompute
