// Copyright 2026 Fable II Recomp contributors.
// Released under the BSD license; see LICENSE in the SDK root.
#include <rex/graphics/frame_limiter.h>
#include <rex/graphics/gpu_diagnostics.h>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rex::graphics {

FrameLimiter::~FrameLimiter() {
#ifdef _WIN32
  if (timer_) CloseHandle(static_cast<HANDLE>(timer_));
#endif
}

void FrameLimiter::Pace(int frames_per_second) {
  if (frames_per_second <= 0 || frames_per_second > 240) {
    rate_ = 0;
    return;
  }
  auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(1.0 / frames_per_second));
  auto now = Clock::now();
  if (rate_ != frames_per_second) {
    rate_ = frames_per_second;
    deadline_ = now + period;
    return;
  }
  if (now < deadline_) {
    WaitUntil(deadline_);
    diag::frame_limiter_wait_us.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - now)
                     .count()),
        std::memory_order_relaxed);
    diag::frame_limiter_waits.fetch_add(1, std::memory_order_relaxed);
    deadline_ += period;
  } else if (now - deadline_ < period / 2) {
    // Keep the schedule, so the next frame gets the time back.
    diag::frame_limiter_late_frames.fetch_add(1, std::memory_order_relaxed);
    deadline_ += period;
  } else {
    diag::frame_limiter_resets.fetch_add(1, std::memory_order_relaxed);
    deadline_ = now + period;
  }
}

void FrameLimiter::WaitUntil(Clock::time_point deadline) {
#ifdef _WIN32
  if (!timer_attempted_) {
    timer_attempted_ = true;
    timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                   CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_MODIFY_STATE | SYNCHRONIZE);
  }
  // No global timer-resolution change or spin loop. Older Windows can fall
  // back to the standard wait, with correspondingly coarser pacing.
  if (timer_) {
    while (Clock::now() < deadline) {
      auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
          deadline - Clock::now()).count();
      if (remaining <= 0) return;
      LARGE_INTEGER due;
      due.QuadPart = -((remaining + 99) / 100);  // Relative 100 ns, rounded up.
      if (!SetWaitableTimer(static_cast<HANDLE>(timer_), &due, 0, nullptr,
                            nullptr, FALSE) ||
          WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE) != WAIT_OBJECT_0) {
        break;
      }
    }
    if (Clock::now() >= deadline) return;
  }
#endif
  std::this_thread::sleep_until(deadline);
}

}  // namespace rex::graphics
