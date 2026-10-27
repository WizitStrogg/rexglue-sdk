/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/gpu_diagnostics.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(shared_memory_upload_prefetch, true, "GPU/D3D12",
                    "When guest memory has to be uploaded for the GPU, also upload the pages the "
                    "GPU used in the previous frame that the CPU has changed since, in the same "
                    "batch, so the following draws don't each need a batch of their own (each "
                    "batch makes the GPU wait for the shared memory buffer transitions)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(shared_memory_upload_prefetch_max_kb, 16384, "GPU/D3D12",
                     "Most memory shared_memory_upload_prefetch uploads with one batch, in KB")
    .range(4, 524288)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(d3d12_tiled_shared_memory, true, "GPU/D3D12",
                    "Use tiled shared memory on D3D12")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::d3d12 {

D3D12SharedMemory::D3D12SharedMemory(D3D12CommandProcessor& command_processor,
                                     memory::Memory& memory)
    : SharedMemory(memory), command_processor_(command_processor) {}

D3D12SharedMemory::~D3D12SharedMemory() {
  Shutdown(true);
}

bool D3D12SharedMemory::Initialize() {
  InitializeCommon();

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, kBufferSize,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (REXCVAR_GET(d3d12_tiled_shared_memory) &&
      provider.GetTiledResourcesTier() != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED &&
      !provider.GetGraphicsAnalysis()) {
    if (FAILED(device->CreateReservedResource(&buffer_desc, buffer_state_, nullptr,
                                              IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB tiled buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
    static_assert(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES == (1 << 16));
    InitializeSparseHostGpuMemory(
        std::max(kHostGpuMemoryOptimalSparseAllocationLog2, uint32_t(16)));
  } else {
    REXGPU_INFO(
        "Direct3D 12 tiled resources are not used for shared memory "
        "emulation - video memory usage may increase significantly "
        "because a full {} MB buffer will be created",
        kBufferSize >> 20);
    if (provider.GetGraphicsAnalysis()) {
      // As of October 8th, 2018, PIX doesn't support tiled buffers.
      // FIXME(Triang3l): Re-enable tiled resources with PIX once fixed.
      REXGPU_INFO(
          "This is caused by PIX being attached, which doesn't support tiled "
          "resources yet.");
    }
    if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                               provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                               buffer_state_, nullptr, IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
  }
  buffer_gpu_address_ = buffer_->GetGPUVirtualAddress();
  buffer_uav_writes_commit_needed_ = false;

  D3D12_DESCRIPTOR_HEAP_DESC buffer_descriptor_heap_desc;
  buffer_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  buffer_descriptor_heap_desc.NumDescriptors = uint32_t(BufferDescriptorIndex::kCount);
  buffer_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  buffer_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&buffer_descriptor_heap_desc,
                                          IID_PPV_ARGS(&buffer_descriptor_heap_)))) {
    REXGPU_ERROR("Shared memory: Failed to create the descriptor heap for buffer views");
    Shutdown();
    return false;
  }
  buffer_descriptor_heap_start_ = buffer_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  ui::d3d12::util::CreateBufferRawSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintSRV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);
  ui::d3d12::util::CreateBufferRawUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintUAV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);

  upload_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, rex::align(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                           size_t(1) << page_size_log2()));

  return true;
}

void D3D12SharedMemory::Shutdown(bool from_destructor) {
  upload_buffer_pool_.reset();

  ui::d3d12::util::ReleaseAndNull(buffer_descriptor_heap_);

  // First free the buffer to detach it from the heaps.
  ui::d3d12::util::ReleaseAndNull(buffer_);

  for (ID3D12Heap* heap : buffer_tiled_heaps_) {
    heap->Release();
  }
  buffer_tiled_heaps_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12SharedMemory::ClearCache() {
  SharedMemory::ClearCache();

  upload_buffer_pool_->ClearCache();
}

void D3D12SharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void D3D12SharedMemory::BeginSubmission() {
  // ExecuteCommandLists is a full UAV barrier.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state) {
  if (buffer_state_ == new_state) {
    if (new_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && buffer_uav_writes_commit_needed_) {
      command_processor_.PushUAVBarrier(buffer_);
      buffer_uav_writes_commit_needed_ = false;
    }
    return;
  }
  command_processor_.PushTransitionBarrier(buffer_, buffer_state_, new_state);
  buffer_state_ = new_state;
  // "UAV -> anything" transition commits the writes implicitly.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::WriteRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintSRV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintSRV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintUAV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintUAV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool D3D12SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                         uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  uint32_t offset_bytes = offset_allocations << host_gpu_memory_sparse_granularity_log2();
  uint32_t length_bytes = length_allocations << host_gpu_memory_sparse_granularity_log2();

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.SizeInBytes = length_bytes;
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | provider.GetHeapFlagCreateNotZeroed();
  ID3D12Heap* heap;
  if (FAILED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)))) {
    REXGPU_ERROR("Shared memory: Failed to create a tile heap");
    return false;
  }
  buffer_tiled_heaps_.push_back(heap);

  D3D12_TILED_RESOURCE_COORDINATE region_start_coordinates;
  region_start_coordinates.X = offset_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_start_coordinates.Y = 0;
  region_start_coordinates.Z = 0;
  region_start_coordinates.Subresource = 0;
  D3D12_TILE_REGION_SIZE region_size;
  region_size.NumTiles = length_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_size.UseBox = FALSE;
  D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_range_start_offset = 0;
  direct_queue->UpdateTileMappings(buffer_, 1, &region_start_coordinates, &region_size, heap, 1,
                                   &range_flags, &heap_range_start_offset, &region_size.NumTiles,
                                   D3D12_TILE_MAPPING_FLAG_NONE);
  command_processor_.NotifyQueueOperationsDoneDirectly();
  return true;
}

void D3D12SharedMemory::UpdateUsedPagesFrame() {
  uint64_t frame = command_processor_.GetCurrentFrame();
  if (used_in_frame_index_ == frame) {
    return;
  }
  size_t block_count = (kBufferSize >> page_size_log2()) >> 6;
  if (used_in_frame_index_ + 1 == frame && used_in_frame_.size() == block_count) {
    prefetch_candidates_.swap(used_in_frame_);
  } else {
    prefetch_candidates_.assign(block_count, 0);
  }
  prefetch_candidate_blocks_.clear();
  for (uint32_t i = 0; i < uint32_t(prefetch_candidates_.size()); ++i) {
    if (prefetch_candidates_[i]) {
      prefetch_candidate_blocks_.push_back(i);
    }
  }
  prefetch_generation_seen_ = UINT64_MAX;
  used_in_frame_.assign(block_count, 0);
  used_in_frame_index_ = frame;
}

void D3D12SharedMemory::RangeAccessedByGpu(uint32_t start, uint32_t length) {
  if (!length || !REXCVAR_GET(shared_memory_upload_prefetch)) {
    return;
  }
  UpdateUsedPagesFrame();
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = std::min((start + (length - 1)) >> page_size_log2(),
                                uint32_t(used_in_frame_.size() << 6) - 1);
  for (uint32_t i = page_first >> 6; i <= page_last >> 6; ++i) {
    uint64_t bits = UINT64_MAX;
    if (i == page_first >> 6) {
      bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == page_last >> 6 && (page_last & 63) != 63) {
      bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    used_in_frame_[i] |= bits;
  }
}

uint32_t D3D12SharedMemory::PrefetchUsedPages() {
  if (!REXCVAR_GET(shared_memory_upload_prefetch) || used_in_frame_.empty() ||
      used_in_frame_index_ + 1 < command_processor_.GetCurrentFrame()) {
    return 0;
  }
  used_prefetch_candidates_ = used_in_frame_;
  used_prefetch_candidate_blocks_.clear();
  for (uint32_t i = 0; i < uint32_t(used_prefetch_candidates_.size()); ++i) {
    if (used_prefetch_candidates_[i]) {
      used_prefetch_candidate_blocks_.push_back(i);
    }
  }
  used_prefetch_ranges_.clear();
  uint32_t max_pages =
      uint32_t(REXCVAR_GET(shared_memory_upload_prefetch_max_kb)) >> (page_size_log2() - 10);
  uint64_t generation_seen = UINT64_MAX;
  uint32_t pages =
      TakeInvalidPages(used_prefetch_candidates_, used_prefetch_candidate_blocks_,
                       std::max(max_pages, uint32_t(1)), used_prefetch_ranges_, generation_seen);
  if (!pages || !UploadRanges(used_prefetch_ranges_)) {
    return 0;
  }
  return pages;
}

bool D3D12SharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& requested_page_ranges) {
  if (requested_page_ranges.empty()) {
    return true;
  }
  // Pages the GPU used in the previous frame and the CPU has changed since go
  // in this batch too, rather than each in a batch of its own later.
  const std::vector<std::pair<uint32_t, uint32_t>>* upload_page_ranges_pointer =
      &requested_page_ranges;
  if (REXCVAR_GET(shared_memory_upload_prefetch)) {
    UpdateUsedPagesFrame();
    // Being uploaded anyway.
    for (const std::pair<uint32_t, uint32_t>& range : requested_page_ranges) {
      for (uint32_t page = range.first; page < range.first + range.second; ++page) {
        if ((page >> 6) < prefetch_candidates_.size()) {
          prefetch_candidates_[page >> 6] &= ~(uint64_t(1) << (page & 63));
        }
      }
    }
    prefetch_ranges_.clear();
    uint32_t prefetch_max_pages =
        uint32_t(REXCVAR_GET(shared_memory_upload_prefetch_max_kb)) >> (page_size_log2() - 10);
    uint32_t prefetched = TakeInvalidPages(
        prefetch_candidates_, prefetch_candidate_blocks_, std::max(prefetch_max_pages, uint32_t(1)),
        prefetch_ranges_, prefetch_generation_seen_);
    if (prefetched) {
      diag::prefetched_pages.fetch_add(prefetched, std::memory_order_relaxed);
      diag::prefetch_batches.fetch_add(1, std::memory_order_relaxed);
      upload_ranges_with_prefetch_.clear();
      std::merge(requested_page_ranges.begin(), requested_page_ranges.end(),
                 prefetch_ranges_.begin(), prefetch_ranges_.end(),
                 std::back_inserter(upload_ranges_with_prefetch_));
      upload_page_ranges_pointer = &upload_ranges_with_prefetch_;
    }
  }
  const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges =
      *upload_page_ranges_pointer;
  D3D12CommandProcessor::GpuProfileScope profile_scope = command_processor_.BeginGpuProfileScope();
  struct ProfileScopeEnd {
    D3D12CommandProcessor& command_processor;
    const D3D12CommandProcessor::GpuProfileScope& scope;
    ~ProfileScopeEnd() {
      command_processor.EndGpuProfileScope(scope, diag::GpuCategory::kMemoryUploads);
    }
  } profile_scope_end{command_processor_, profile_scope};
  CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.SubmitBarriers();
  auto& command_list = command_processor_.GetDeferredCommandList();
  diag::upload_batches.fetch_add(1, std::memory_order_relaxed);
  const size_t upload_reason_index = size_t(diag::upload_reason);
  diag::upload_batches_by_reason[upload_reason_index].fetch_add(1, std::memory_order_relaxed);
  auto count_upload_bytes = [upload_reason_index](size_t size) {
    diag::upload_bytes.fetch_add(size, std::memory_order_relaxed);
    diag::upload_bytes_by_reason[upload_reason_index].fetch_add(size, std::memory_order_relaxed);
  };
  {
    // Pages uploaded more than once in a frame.
    uint64_t frame = command_processor_.GetCurrentFrame();
    if (uploaded_in_frame_index_ != frame) {
      uploaded_in_frame_index_ = frame;
      uploaded_in_frame_.assign((kBufferSize >> page_size_log2()) >> 6, 0);
    }
    uint64_t reuploaded = 0;
    for (const std::pair<uint32_t, uint32_t>& upload_range : upload_page_ranges) {
      for (uint32_t page = upload_range.first; page < upload_range.first + upload_range.second;
           ++page) {
        uint64_t& block = uploaded_in_frame_[page >> 6];
        uint64_t bit = uint64_t(1) << (page & 63);
        reuploaded += (block & bit) != 0;
        block |= bit;
      }
    }
    if (reuploaded) {
      diag::pages_reuploaded_in_frame.fetch_add(reuploaded, std::memory_order_relaxed);
    }
  }

  // Copies [start, start + length) (dword-aligned) from guest memory.
  auto upload_bytes = [&](uint32_t start, uint32_t length) -> bool {
    while (length != 0) {
      ID3D12Resource* upload_buffer;
      size_t upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), length, sizeof(uint32_t), &upload_buffer,
          &upload_buffer_offset, &upload_buffer_size, nullptr);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get an upload buffer");
        return false;
      }
      std::memcpy(upload_buffer_mapping, memory().TranslatePhysical(start), upload_buffer_size);
      command_list.D3DCopyBufferRegion(buffer_, start, upload_buffer, UINT64(upload_buffer_offset),
                                       UINT64(upload_buffer_size));
      count_upload_bytes(upload_buffer_size);
      start += uint32_t(upload_buffer_size);
      length -= uint32_t(upload_buffer_size);
    }
    return true;
  };

  auto upload_pages = [&](uint32_t upload_range_start, uint32_t upload_range_length) -> bool {
    // Where memexport results haven't been read back yet, guest memory is
    // older than the GPU's data, so keep the GPU's data there, as an eager
    // readback would have overwritten guest memory with it already.
    upload_kept_ranges_.clear();
    command_processor_.GetUnreadMemexportRanges(upload_range_start << page_size_log2(),
                                                    upload_range_length << page_size_log2(),
                                                    upload_kept_ranges_);
    if (!upload_kept_ranges_.empty()) {
      uint32_t start = upload_range_start << page_size_log2();
      uint32_t end = (upload_range_start + upload_range_length) << page_size_log2();
      MakeRangeValid(start, end - start, false);
      uint32_t kept_bytes = 0;
      for (const std::pair<uint32_t, uint32_t>& kept_range : upload_kept_ranges_) {
        // Still GPU-written data, so CPU writes there are handled as such.
        uint32_t kept_page_start = kept_range.first & ~((uint32_t(1) << page_size_log2()) - 1);
        MakeRangeValid(kept_page_start, kept_range.second - kept_page_start, true);
        kept_bytes += kept_range.second - kept_range.first;
      }
      diag::uploads_keeping_readback_data.fetch_add(1, std::memory_order_relaxed);
      diag::upload_bytes_kept_for_readback.fetch_add(kept_bytes, std::memory_order_relaxed);
      for (const std::pair<uint32_t, uint32_t>& kept_range : upload_kept_ranges_) {
        if (kept_range.first > start && !upload_bytes(start, kept_range.first - start)) {
          return false;
        }
        start = kept_range.second;
      }
      return start >= end || upload_bytes(start, end - start);
    }
    while (upload_range_length != 0) {
      ID3D12Resource* upload_buffer;
      size_t upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), &upload_buffer, &upload_buffer_offset, &upload_buffer_size,
          nullptr);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get an upload buffer");
        return false;
      }
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      std::memcpy(upload_buffer_mapping,
                  memory().TranslatePhysical(upload_range_start << page_size_log2()),
                  upload_buffer_size);
      command_list.D3DCopyBufferRegion(buffer_, upload_range_start << page_size_log2(),
                                       upload_buffer, UINT64(upload_buffer_offset),
                                       UINT64(upload_buffer_size));
      count_upload_bytes(upload_buffer_size);
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
    return true;
  };

  // Uploads only the dwords of the page that differ from the snapshot taken
  // before the CPU wrote to it, keeping the GPU-written data in the rest.
  auto upload_page_merged = [&](uint32_t page, const std::vector<uint32_t>& snapshot) -> bool {
    uint32_t page_address = page << page_size_log2();
    uint32_t page_dwords = uint32_t(snapshot.size());
    MakeRangeValid(page_address, uint32_t(1) << page_size_log2(), true);
    const uint32_t* current =
        reinterpret_cast<const uint32_t*>(memory().TranslatePhysical(page_address));
    // Guest memory is stale where memexport results are unread (see
    // upload_pages).
    upload_kept_ranges_.clear();
    command_processor_.GetUnreadMemexportRanges(page_address, uint32_t(1) << page_size_log2(),
                                                upload_kept_ranges_);
    if (!upload_kept_ranges_.empty()) {
      diag::uploads_keeping_readback_data.fetch_add(1, std::memory_order_relaxed);
    }
    auto is_kept = [&](uint32_t dword) -> bool {
      uint32_t address = page_address + dword * sizeof(uint32_t);
      for (const std::pair<uint32_t, uint32_t>& kept_range : upload_kept_ranges_) {
        if (address >= kept_range.first && address < kept_range.second) {
          return true;
        }
      }
      return false;
    };
    auto is_changed = [&](uint32_t dword) -> bool {
      return current[dword] != snapshot[dword] && !is_kept(dword);
    };
    uint32_t i = 0;
    while (i < page_dwords) {
      if (!is_changed(i)) {
        ++i;
        continue;
      }
      uint32_t run_end = i + 1;
      while (run_end < page_dwords && is_changed(run_end)) {
        ++run_end;
      }
      diag::merged_dwords.fetch_add(run_end - i, std::memory_order_relaxed);
      while (i < run_end) {
        ID3D12Resource* upload_buffer;
        size_t upload_buffer_offset, upload_buffer_size;
        uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
            command_processor_.GetCurrentSubmission(), (run_end - i) * sizeof(uint32_t),
            sizeof(uint32_t), &upload_buffer, &upload_buffer_offset, &upload_buffer_size, nullptr);
        if (upload_buffer_mapping == nullptr) {
          REXGPU_ERROR("Shared memory: Failed to get an upload buffer");
          return false;
        }
        std::memcpy(upload_buffer_mapping, current + i, upload_buffer_size);
        command_list.D3DCopyBufferRegion(buffer_, page_address + i * sizeof(uint32_t),
                                         upload_buffer, UINT64(upload_buffer_offset),
                                         UINT64(upload_buffer_size));
        count_upload_bytes(upload_buffer_size);
        i += uint32_t(upload_buffer_size / sizeof(uint32_t));
      }
    }
    diag::merged_page_uploads.fetch_add(1, std::memory_order_relaxed);
    return true;
  };

  std::vector<uint32_t> snapshot;
  for (auto upload_range : upload_page_ranges) {
    if (!HasCpuWriteSnapshots()) {
      if (!upload_pages(upload_range.first, upload_range.second)) {
        return false;
      }
      continue;
    }
    uint32_t run_start = upload_range.first;
    uint32_t range_end = upload_range.first + upload_range.second;
    for (uint32_t page = upload_range.first; page < range_end; ++page) {
      if (!TakeCpuWriteSnapshot(page, snapshot)) {
        continue;
      }
      if (page > run_start && !upload_pages(run_start, page - run_start)) {
        return false;
      }
      if (!upload_page_merged(page, snapshot)) {
        return false;
      }
      run_start = page + 1;
    }
    if (range_end > run_start && !upload_pages(run_start, range_end - run_start)) {
      return false;
    }
  }
  return true;
}

}  // namespace rex::graphics::d3d12
