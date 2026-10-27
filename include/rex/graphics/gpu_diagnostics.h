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

// Batches of guest memory uploads (each a pair of shared memory barriers), and
// the bytes uploaded.
inline std::atomic<uint64_t> upload_batches{0};
inline std::atomic<uint64_t> upload_bytes{0};
// What the uploads are for, set around the shared memory requests.
enum class UploadReason : uint32_t {
  kVertices,
  kIndices,
  kMemexport,
  kTextures,
  kResolves,
  kOther,
  kCount,
};
inline thread_local UploadReason upload_reason = UploadReason::kOther;
inline std::array<std::atomic<uint64_t>, size_t(UploadReason::kCount)> upload_batches_by_reason{};
inline std::array<std::atomic<uint64_t>, size_t(UploadReason::kCount)> upload_bytes_by_reason{};
class ScopedUploadReason {
 public:
  explicit ScopedUploadReason(UploadReason reason) : previous_(upload_reason) {
    upload_reason = reason;
  }
  ~ScopedUploadReason() { upload_reason = previous_; }
  ScopedUploadReason(const ScopedUploadReason&) = delete;
  ScopedUploadReason& operator=(const ScopedUploadReason&) = delete;

 private:
  UploadReason previous_;
};
// Pages uploaded again in the same guest frame.
inline std::atomic<uint64_t> pages_reuploaded_in_frame{0};
// shared_memory_upload_prefetch: pages the GPU used in the previous frame
// uploaded along with a batch before being requested, and the batches.
inline std::atomic<uint64_t> prefetched_pages{0};
inline std::atomic<uint64_t> prefetch_batches{0};
// Invalidations of shared memory by CPU writes (and other guest memory
// changes), and the pages invalidated (with the expansion around them).
inline std::atomic<uint64_t> cpu_invalidations{0};
inline std::atomic<uint64_t> cpu_invalidated_pages{0};
// Pages holding GPU-written (memexport/resolve) data that the CPU wrote to.
inline std::atomic<uint64_t> cpu_writes_to_gpu_pages{0};
// Pages uploaded by merging only the dwords the CPU changed.
inline std::atomic<uint64_t> merged_page_uploads{0};
inline std::atomic<uint64_t> merged_dwords{0};
// Uploads that kept the GPU's data where a pending lazy readback is still to
// overwrite stale guest memory, and the bytes kept.
inline std::atomic<uint64_t> uploads_keeping_readback_data{0};
inline std::atomic<uint64_t> upload_bytes_kept_for_readback{0};
// Uploads over memexport results written since the last readback (kept or
// not, per readback_memexport_keep_unread), and the bytes.
inline std::atomic<uint64_t> uploads_over_unread_memexport{0};
inline std::atomic<uint64_t> upload_bytes_over_unread_memexport{0};
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
// readback_memexport_lazy: guest memory the command processor reads to
// execute it, overlapping memexport results still being copied back, or not
// copied back yet.
enum class CpMemoryRead : uint32_t {
  kConstants,
  kShader,
  kIndirectBuffer,
  kCount,
};
inline std::array<std::atomic<uint64_t>, size_t(CpMemoryRead::kCount)>
    cp_reads_of_pending_memexport{};
inline std::array<std::atomic<uint64_t>, size_t(CpMemoryRead::kCount)>
    cp_reads_of_unissued_memexport{};
// readback_memexport_lazy_apply_thread: copies put in guest memory as soon as
// the GPU completed them.
inline std::atomic<uint64_t> lazy_readbacks_applied_by_thread{0};
// Command processor time spent waiting for the GPU, in microseconds.
inline std::atomic<uint64_t> cp_gpu_wait_us{0};
// What the command processor was doing when it waited for the GPU, set around
// the callers.
enum class CpGpuWait : uint32_t {
  // Before a WAIT_REG_MEM, for the queued guest-visible writes.
  kWaitRegMem,
  // Out of commands, for the queued guest-visible writes.
  kIdle,
  // Before a REG_TO_MEM or COND_WRITE, for the queued guest-visible writes.
  kRegToMem,
  // Memexport results read back synchronously.
  kReadback,
  // Opening a frame with kQueueFrames frames already in flight.
  kFrameQueue,
  kOther,
  kCount,
};
inline thread_local CpGpuWait cp_gpu_wait = CpGpuWait::kOther;
inline std::array<std::atomic<uint64_t>, size_t(CpGpuWait::kCount)> cp_gpu_wait_us_by_reason{};
class ScopedCpGpuWait {
 public:
  explicit ScopedCpGpuWait(CpGpuWait reason) : previous_(cp_gpu_wait) { cp_gpu_wait = reason; }
  ~ScopedCpGpuWait() { cp_gpu_wait = previous_; }
  ScopedCpGpuWait(const ScopedCpGpuWait&) = delete;
  ScopedCpGpuWait& operator=(const ScopedCpGpuWait&) = delete;

 private:
  CpGpuWait previous_;
};
// d3d12_submission_interval_draws: submissions ended for the interval, and
// draws that reached it but couldn't end the submission yet.
inline std::atomic<uint64_t> interval_submissions{0};
inline std::atomic<uint64_t> interval_submissions_deferred{0};
// Memexport results read back with the command processor waiting for the GPU.
inline std::atomic<uint64_t> sync_readbacks{0};
// readback_memexport_async: readbacks, those issued after
// readback_memexport_eager_draws draws, and the draws without memexport between
// the last memexport draw and each readback (GPU work the guest-visible writes
// queued behind the readback also wait for).
inline std::atomic<uint64_t> async_readbacks{0};
inline std::atomic<uint64_t> eager_readbacks{0};
inline std::atomic<uint64_t> draws_after_memexport_at_readback{0};
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
// Guest frames (swap to swap) longer than 20, 33 and 50 ms, and the longest.
inline std::atomic<uint64_t> frames_over_20ms{0};
inline std::atomic<uint64_t> frames_over_33ms{0};
inline std::atomic<uint64_t> frames_over_50ms{0};
inline std::atomic<uint64_t> longest_frame_us{0};
inline void RecordFrameTime(uint64_t frame_us) {
  if (frame_us > 20000) {
    frames_over_20ms.fetch_add(1, std::memory_order_relaxed);
  }
  if (frame_us > 33333) {
    frames_over_33ms.fetch_add(1, std::memory_order_relaxed);
  }
  if (frame_us > 50000) {
    frames_over_50ms.fetch_add(1, std::memory_order_relaxed);
  }
  uint64_t longest = longest_frame_us.load(std::memory_order_relaxed);
  while (frame_us > longest &&
         !longest_frame_us.compare_exchange_weak(longest, frame_us, std::memory_order_relaxed)) {
  }
}

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

inline std::string TakeUploadReasonSummary() {
  std::string summary;
  for (size_t i = 0; i < size_t(UploadReason::kCount); ++i) {
    summary += fmt::format("{}{}/{} KB", i ? " " : "", upload_batches_by_reason[i].exchange(0),
                           upload_bytes_by_reason[i].exchange(0) >> 10);
  }
  return summary;
}

inline std::string TakeLazyCpReadSummary() {
  std::string summary;
  for (size_t i = 0; i < size_t(CpMemoryRead::kCount); ++i) {
    summary += fmt::format("{}{}", i ? "/" : "", cp_reads_of_pending_memexport[i].exchange(0));
  }
  summary += ", not copied back yet ";
  for (size_t i = 0; i < size_t(CpMemoryRead::kCount); ++i) {
    summary += fmt::format("{}{}", i ? "/" : "", cp_reads_of_unissued_memexport[i].exchange(0));
  }
  return summary;
}

inline std::string TakeCpGpuWaitSummary() {
  std::string summary =
      "command processor GPU waits by cause (WAIT_REG_MEM/idle/REG_TO_MEM/readback/frame "
      "queue/other) ";
  for (size_t i = 0; i < size_t(CpGpuWait::kCount); ++i) {
    summary += fmt::format("{}{}", i ? "/" : "", cp_gpu_wait_us_by_reason[i].exchange(0) / 1000);
  }
  summary += fmt::format(" ms, synchronous readbacks {}, interval submissions {} ({} deferred)",
                         sync_readbacks.exchange(0), interval_submissions.exchange(0),
                         interval_submissions_deferred.exchange(0));
  summary +=
      fmt::format(", asynchronous readbacks {} ({} eager) after {} draws without memexport in all",
                  async_readbacks.exchange(0), eager_readbacks.exchange(0),
                  draws_after_memexport_at_readback.exchange(0));
  return summary;
}

inline std::string TakeSummary() {
  return fmt::format(
      "memexport {} draws / {} KB, readback {} KB, uploads {} batches / {} KB (by reason, "
      "vertices/indices/memexport/textures/resolves/other: {}; pages uploaded again in the "
      "frame {}; prefetched {} pages in {} batches), CPU write invalidations {} ({} pages), CPU writes to "
      "GPU-written pages {}, merged uploads {} pages / {} dwords, uploads keeping pending "
      "readback data {} ({} KB), uploads over unread memexport results {} ({} KB), {} "
      "submissions, GPU busy {} ms, command processor GPU "
      "waits {} ms (of the GPU time: render target transfers {} ms, texture loads {} ms, "
      "resolves {} ms, memory uploads {} ms; transfers: host depth store {} ms, barriers {} ms, "
      "draws {} ms, stencil clears {} ms, stencil bit draws {} ms), submission thread {} ms "
      "(command processor waited {} ms for it), lazy readbacks {}, guest accesses to pages "
      "being read back {} ({} waited, {} ms), command processor reads of them {} "
      "(constants/shaders/indirect buffers {}), copies applied by the apply thread {}, frame "
      "limiter waited {} ms in {} frames ({} late, {} resets), command processor idle {} ms, "
      "frames over 20/33/50 ms {}/{}/{}, longest {} ms",
      memexport_draws.exchange(0), memexport_bytes.exchange(0) >> 10,
      readback_bytes.exchange(0) >> 10, upload_batches.exchange(0), upload_bytes.exchange(0) >> 10,
      TakeUploadReasonSummary(), pages_reuploaded_in_frame.exchange(0),
      prefetched_pages.exchange(0), prefetch_batches.exchange(0),
      cpu_invalidations.exchange(0), cpu_invalidated_pages.exchange(0),
      cpu_writes_to_gpu_pages.exchange(0),
      merged_page_uploads.exchange(0), merged_dwords.exchange(0),
      uploads_keeping_readback_data.exchange(0), upload_bytes_kept_for_readback.exchange(0) >> 10,
      uploads_over_unread_memexport.exchange(0), upload_bytes_over_unread_memexport.exchange(0) >> 10,
      submissions.exchange(0),
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
      TakeLazyCpReadSummary(), lazy_readbacks_applied_by_thread.exchange(0),
      frame_limiter_wait_us.exchange(0) / 1000, frame_limiter_waits.exchange(0),
      frame_limiter_late_frames.exchange(0), frame_limiter_resets.exchange(0),
      cp_idle_us.exchange(0) / 1000, frames_over_20ms.exchange(0), frames_over_33ms.exchange(0),
      frames_over_50ms.exchange(0), longest_frame_us.exchange(0) / 1000) +
         "; " + TakeTransferSummary() + "; " + TakeCpGpuWaitSummary();
}

}  // namespace rex::graphics::diag
