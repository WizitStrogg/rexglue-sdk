#pragma once
/**
 * Lightweight counters for diagnosing GPU emulation performance, summed over
 * the logging interval of the command processor's guest-visible write log.
 */

#include <array>
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
// readback_memexport_lazy: copies issued, guest accesses to pages still being
// copied back, those that had to wait for the GPU and the time they waited,
// and command processor reads of such memory.
inline std::atomic<uint64_t> lazy_readbacks{0};
inline std::atomic<uint64_t> lazy_page_accesses{0};
inline std::atomic<uint64_t> lazy_page_access_waits{0};
inline std::atomic<uint64_t> lazy_page_access_wait_us{0};
inline std::atomic<uint64_t> lazy_gpu_reads{0};
// Command processor time spent waiting for the GPU, in microseconds.
inline std::atomic<uint64_t> cp_gpu_wait_us{0};
// Time the submission thread spent replaying and submitting command lists, and
// time the command processor spent waiting for it.
inline std::atomic<uint64_t> submission_thread_us{0};
inline std::atomic<uint64_t> submission_thread_flush_us{0};
// Frame limiter: time spent waiting for the next frame's slot and the frames
// that waited, frames that came in late but kept the schedule, and frames so
// late that the schedule restarted.
inline std::atomic<uint64_t> frame_limiter_wait_us{0};
inline std::atomic<uint64_t> frame_limiter_waits{0};
inline std::atomic<uint64_t> frame_limiter_late_frames{0};
inline std::atomic<uint64_t> frame_limiter_resets{0};
// Command processor time with no guest commands to execute, in microseconds.
inline std::atomic<uint64_t> cp_idle_us{0};

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
// Depth transfers whose stencil the destination already holds.
inline std::atomic<uint64_t> stencil_unchanged_transfers{0};
// EDRAM tiles of transfers to depth by why the stencil had to be copied:
// 0 - kept (not copied), 1 - color write, 2 - draw writing stencil, 3 - clear,
// 4 - held only by other depth render targets, 5 - came from color with no
// depth holding it, 6 - no tracking, 7 - never tracked.
inline std::array<std::atomic<uint64_t>, 8> stencil_transfer_tiles_by_reason{};
inline void RecordStencilTransfer(uint32_t reason, uint32_t tiles) {
  stencil_transfer_tiles_by_reason[reason & 7].fetch_add(tiles, std::memory_order_relaxed);
}
// Draws to depth that may change the stencil.
inline std::atomic<uint64_t> stencil_write_draws{0};
// Draws with depth bound by [1x, 4x MSAA][stencil disabled, read only, writes].
inline std::atomic<uint64_t> depth_draws_by_stencil_use[2][3];
// OR of the stencil bits draws may set.
inline std::atomic<uint32_t> draw_stencil_bits_or{0};
// EDRAM tiles of depth transfers needing stencil bit draws by the stencil bits.
inline std::atomic<uint64_t> stencil_transfer_tiles_by_bits[256];
inline void RecordStencilTransferBits(uint32_t bits, uint32_t tiles) {
  stencil_transfer_tiles_by_bits[bits & 0xFF].fetch_add(tiles, std::memory_order_relaxed);
}
// EDRAM tiles of transfers back to a render target still holding the data.
inline std::atomic<uint64_t> redundant_transfer_tiles{0};
inline std::string TakeTransferSummary() {
  std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> entries(transfer_stats.begin(),
                                                                          transfer_stats.end());
  transfer_stats.clear();
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
  std::string result = "stencil to depth tiles by reason (kept/color write/stencil draw/clear/"
                       "other depth/from color/untracked/never):";
  for (auto& reason_tiles : stencil_transfer_tiles_by_reason) {
    result += fmt::format(" {}", reason_tiles.exchange(0));
  }
  result += fmt::format("; stencil-writing draws {}", stencil_write_draws.exchange(0));
  result += "; depth draws 1x/4x by stencil (off/read/write):";
  for (auto& msaa_draws : depth_draws_by_stencil_use) {
    result += fmt::format(" {}/{}/{}", msaa_draws[0].exchange(0), msaa_draws[1].exchange(0),
                          msaa_draws[2].exchange(0));
  }
  result += fmt::format("; draw stencil bits OR {:02X}; stencil transfer tiles by bits:",
                        draw_stencil_bits_or.exchange(0));
  {
    std::vector<std::pair<uint64_t, uint32_t>> bits_tiles;
    for (uint32_t i = 0; i < 256; ++i) {
      uint64_t tiles = stencil_transfer_tiles_by_bits[i].exchange(0);
      if (tiles) {
        bits_tiles.emplace_back(tiles, i);
      }
    }
    std::sort(bits_tiles.begin(), bits_tiles.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < std::min(bits_tiles.size(), size_t(8)); ++i) {
      result += fmt::format(" {:02X}:{}", bits_tiles[i].second, bits_tiles[i].first);
    }
  }
  result += "; ";
  result +=
      fmt::format("stencil bit draws {} done, {} skipped; stencil kept in {} transfers; "
                  "redundant transfers {} tiles; render target transfers by EDRAM tiles:",
                  stencil_bit_transfers.exchange(0), stencil_bit_transfers_skipped.exchange(0),
                  stencil_unchanged_transfers.exchange(0), redundant_transfer_tiles.exchange(0));
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
      "(command processor waited {} ms for it), lazy readbacks {}, guest accesses to pages "
      "being read back {} ({} waited, {} ms), command processor reads of them {}, frame "
      "limiter waited {} ms in {} frames ({} late, {} resets), command processor idle {} ms",
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
      submission_thread_us.exchange(0) / 1000, submission_thread_flush_us.exchange(0) / 1000,
      lazy_readbacks.exchange(0), lazy_page_accesses.exchange(0), lazy_page_access_waits.exchange(0),
      lazy_page_access_wait_us.exchange(0) / 1000, lazy_gpu_reads.exchange(0),
      frame_limiter_wait_us.exchange(0) / 1000, frame_limiter_waits.exchange(0),
      frame_limiter_late_frames.exchange(0), frame_limiter_resets.exchange(0),
      cp_idle_us.exchange(0) / 1000) +
         "; " + TakeTransferSummary();
}

}  // namespace rex::graphics::diag
