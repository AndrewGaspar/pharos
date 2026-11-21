#include "ports/bifrost_rate.h"
#include <algorithm>
#include "core/log.h"

namespace bifrost {
void FrameRate::configure(unsigned low, unsigned high) {
  minimum = low;
  maximum = high;
  multiplier = std::clamp(multiplier, low, high);
  cancel();
}
void FrameRate::cancel() {
  start = previous = {};
  frames = slow = fast = 0;
  sleep = {};
}
void FrameRate::frame(Duration refresh) {
  if (minimum == maximum || refresh <= Duration::zero()) return;
  const auto now = Clock::now();
  if (previous == Clock::time_point{} || now - previous > std::chrono::milliseconds(250)) {
    cancel();
    start = previous = now;
    return;
  }
  previous = now;
  ++frames;
  const auto elapsed = now - start;
  if (elapsed < std::chrono::seconds(1)) return;
  const double work = std::chrono::duration<double>(elapsed - std::min(sleep, elapsed)).count() / frames;
  const double interval = std::chrono::duration<double>(elapsed).count() / frames;
  const double display = std::chrono::duration<double>(refresh).count();
  const auto old = multiplier;
  slow = multiplier < maximum && interval > display * multiplier / .90 ? slow + 1 : 0;
  fast = multiplier > minimum && now >= hold_down && work < .85 * display * (multiplier - 1) ? fast + 1 : 0;
  if (slow >= 2) {
    ++multiplier;
    if (now - last_drop < std::chrono::seconds(6)) {
      hold_down = now + std::chrono::seconds(backoff);
      backoff = std::min(60u, backoff * 2);
    }
  } else if (fast >= 3) {
    --multiplier;
    last_drop = now;
  }
  if (old != multiplier) {
    slow = fast = 0;
    ACO_LOG("Bifrost: automatic FG %ux -> %ux, real %.1f fps, %.1f ms per frame excluding pacing, target %.1f Hz",
            old, multiplier, 1. / interval, 1000 * work, 1. / display);
  }
  if (now - last_drop > std::chrono::seconds(20)) backoff = 10;
  frames = 0;
  sleep = {};
  start = now;
}
}  // namespace bifrost
