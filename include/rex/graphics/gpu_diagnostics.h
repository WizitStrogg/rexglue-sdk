#pragma once
/**
 * Lightweight counters for diagnosing GPU emulation performance, summed over
 * the logging interval of the command processor's guest-visible write log.
 */

#include <atomic>
#include <cstdint>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace rex::graphics::diag {

// Pages holding GPU-written (memexport/resolve) data that the CPU wrote to.
inline std::atomic<uint64_t> cpu_writes_to_gpu_pages{0};
// Pages uploaded by merging only the dwords the CPU changed.
inline std::atomic<uint64_t> merged_page_uploads{0};
inline std::atomic<uint64_t> merged_dwords{0};
// Draws with memory export, and the bytes they export (known ranges only).
inline std::atomic<uint64_t> memexport_draws{0};
inline std::atomic<uint64_t> memexport_bytes{0};
// Bytes copied back for the CPU.
inline std::atomic<uint64_t> readback_bytes{0};
// Host GPU time spent executing submissions, in microseconds, and the count.
inline std::atomic<uint64_t> gpu_busy_us{0};
inline std::atomic<uint64_t> submissions{0};
// Host GPU time of categories of emulation work within the submissions, in
// microseconds (the rest is mostly the guest's own draws).
enum class GpuCategory : uint32_t {
  kRenderTargetTransfers,
  kTextureLoads,
  kResolves,
  kMemoryUploads,
  // Parts of render target transfers (within kRenderTargetTransfers).
  kTransferHostDepthStore,
  kTransferBarriers,
  kTransferDraws,
  kTransferStencilClears,
  kTransferStencilDraws,
  kCount,
};
inline std::atomic<uint64_t> gpu_category_us[size_t(GpuCategory::kCount)];
// Command processor time spent waiting for the GPU, in microseconds.
inline std::atomic<uint64_t> cp_gpu_wait_us{0};
// Time the submission thread spent replaying and submitting command lists, and
// time the command processor spent waiting for it.
inline std::atomic<uint64_t> submission_thread_us{0};
inline std::atomic<uint64_t> submission_thread_flush_us{0};

// Render target ownership transfers (EDRAM data copied between host render
// targets): count and EDRAM tiles, by source and destination render target
// (key bits other than the base and the pitch). Command processor thread only.
inline std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> transfer_stats;
inline void RecordTransfer(uint32_t source_key, uint32_t dest_key, uint32_t tiles) {
  // Drop the EDRAM base (11 bits) and the pitch (8 bits).
  uint64_t key = (uint64_t(source_key >> 19) << 32) | (dest_key >> 19);
  auto& stats = transfer_stats[key];
  ++stats.first;
  stats.second += tiles;
}
// Formats the format names of a key without the base and the pitch, set by the
// render target cache.
inline std::string (*describe_render_target_key)(uint32_t key_without_base_pitch) = nullptr;
// Stencil bit draws of depth transfers (8 per transfer without stencil
// reference output), done and skipped because the bit is zero in the source.
inline std::atomic<uint64_t> stencil_bit_transfers{0};
inline std::atomic<uint64_t> stencil_bit_transfers_skipped{0};
// EDRAM tiles of transfers back to a render target still holding the data.
inline std::atomic<uint64_t> redundant_transfer_tiles{0};
inline std::string TakeTransferSummary() {
  std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> entries(transfer_stats.begin(),
                                                                          transfer_stats.end());
  transfer_stats.clear();
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
  std::string result =
      fmt::format("stencil bit draws {} done, {} skipped; redundant transfers {} "
                  "tiles; render target transfers by EDRAM tiles:",
                  stencil_bit_transfers.exchange(0), stencil_bit_transfers_skipped.exchange(0),
                  redundant_transfer_tiles.exchange(0));
  for (size_t i = 0; i < std::min(entries.size(), size_t(8)); ++i) {
    uint32_t source = uint32_t(entries[i].first >> 32);
    uint32_t dest = uint32_t(entries[i].first);
    result += fmt::format(" [{} -> {}: {} transfers, {} tiles]",
                          describe_render_target_key ? describe_render_target_key(source) : "?",
                          describe_render_target_key ? describe_render_target_key(dest) : "?",
                          entries[i].second.first, entries[i].second.second);
  }
  return result;
}

inline std::string TakeSummary() {
  return fmt::format(
      "memexport {} draws / {} KB, readback {} KB, CPU writes to GPU-written pages {}, merged "
      "uploads {} pages / {} dwords, {} submissions, GPU busy {} ms, command processor GPU "
      "waits {} ms (of the GPU time: render target transfers {} ms, texture loads {} ms, "
      "resolves {} ms, memory uploads {} ms; transfers: host depth store {} ms, barriers {} ms, "
      "draws {} ms, stencil clears {} ms, stencil bit draws {} ms), submission thread {} ms "
      "(command processor waited {} ms for it)",
      memexport_draws.exchange(0), memexport_bytes.exchange(0) >> 10,
      readback_bytes.exchange(0) >> 10, cpu_writes_to_gpu_pages.exchange(0),
      merged_page_uploads.exchange(0), merged_dwords.exchange(0), submissions.exchange(0),
      gpu_busy_us.exchange(0) / 1000, cp_gpu_wait_us.exchange(0) / 1000,
      gpu_category_us[size_t(GpuCategory::kRenderTargetTransfers)].exchange(0) / 1000,
      gpu_category_us[size_t(GpuCategory::kTextureLoads)].exchange(0) / 1000,
      gpu_category_us[size_t(GpuCategory::kResolves)].exchange(0) / 1000,
      gpu_category_us[size_t(GpuCategory::kMemoryUploads)].exchange(0) / 1000,
      gpu_category_us[size_t(GpuCategory::kTransferHostDepthStore)].exchange(0) / 1000,
      gpu_category_us[size_t(GpuCategory::kTransferBarriers)].exchange(0) / 1000,
      gpu_category_us[size_t(GpuCategory::kTransferDraws)].exchange(0) / 1000,
      gpu_category_us[size_t(GpuCategory::kTransferStencilClears)].exchange(0) / 1000,
      gpu_category_us[size_t(GpuCategory::kTransferStencilDraws)].exchange(0) / 1000,
      submission_thread_us.exchange(0) / 1000, submission_thread_flush_us.exchange(0) / 1000) +
         "; " + TakeTransferSummary();
}

}  // namespace rex::graphics::diag
