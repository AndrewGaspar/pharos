#pragma once

#include <chrono>

namespace bifrost {
// Feedback for Valhalla's native presentation queue. The clock runs between
// consecutive real frames, including every generated flip in that interval.
class FrameRate {
 public:
  using Clock = std::chrono::steady_clock;
  using Duration = Clock::duration;
  void configure(unsigned low, unsigned high);
  void cancel();
  void slept(Duration time) { sleep += time; }
  unsigned count() const { return multiplier; }
  unsigned capacity() const { return maximum; }
  void frame(Duration refresh);

 private:
  unsigned minimum = 2, maximum = 2, multiplier = 2;
  unsigned frames{}, slow{}, fast{};
  Duration sleep{};
  Clock::time_point start{}, previous{}, last_drop{}, hold_down{};
  unsigned backoff = 10;
};
}  // namespace bifrost
