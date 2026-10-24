#pragma once
/**
 * Lightweight counters for diagnosing GPU emulation performance, summed over
 * the logging interval of the command processor's guest-visible write log.
 */

#include <atomic>
#include <cstdint>
#include <string>

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
// Command processor time spent waiting for the GPU, in microseconds.
inline std::atomic<uint64_t> cp_gpu_wait_us{0};

inline std::string TakeSummary() {
  return fmt::format(
      "memexport {} draws / {} KB, readback {} KB, CPU writes to GPU-written pages {}, merged "
      "uploads {} pages / {} dwords, {} submissions, GPU busy {} ms, command processor GPU "
      "waits {} ms",
      memexport_draws.exchange(0), memexport_bytes.exchange(0) >> 10,
      readback_bytes.exchange(0) >> 10, cpu_writes_to_gpu_pages.exchange(0),
      merged_page_uploads.exchange(0), merged_dwords.exchange(0), submissions.exchange(0),
      gpu_busy_us.exchange(0) / 1000, cp_gpu_wait_us.exchange(0) / 1000);
}

}  // namespace rex::graphics::diag
