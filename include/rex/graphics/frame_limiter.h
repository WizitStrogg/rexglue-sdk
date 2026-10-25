// Copyright 2026 Fable II Recomp contributors.
// Released under the BSD license; see LICENSE in the SDK root.
#pragma once

#include <chrono>

namespace rex::graphics {

// Worker-thread-only host swap limiter. It neither changes the guest clock nor
// advertises a different video mode. Swaps follow a fixed schedule, so a late
// wake-up or a slightly late frame is made up on the next frame instead of
// lowering the rate. Frames more than half a period late restart the schedule
// instead of bursting.
class FrameLimiter {
 public:
  FrameLimiter() = default;
  ~FrameLimiter();
  FrameLimiter(const FrameLimiter&) = delete;
  FrameLimiter& operator=(const FrameLimiter&) = delete;
  void Pace(int frames_per_second);

 private:
  using Clock = std::chrono::steady_clock;
  void WaitUntil(Clock::time_point deadline);
  void* timer_ = nullptr;
  bool timer_attempted_ = false;
  // When the next swap may happen.
  Clock::time_point deadline_{};
  int rate_ = 0;
};

}  // namespace rex::graphics
