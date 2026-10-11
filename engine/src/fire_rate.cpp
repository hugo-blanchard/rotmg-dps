#include "rotmg/fire_rate.hpp"

#include <algorithm>

namespace rotmg {

double AttacksPerSecond(int dexterity, const PlayerStatus& status) {
  if (status.dazed) {
    return 1.5;
  }
  const double rate = 1.5 + 6.5 * dexterity / 75.0;
  return status.berserk ? rate * 1.25 : rate;
}

double BurstDelay(const Subattack& subattack, int dexterity, const PlayerStatus& status) {
  const double max_delay = subattack.burst_delay.value_or(0);
  const double min_delay = subattack.burst_min_delay.value_or(max_delay);
  const int dex = status.dazed ? 0 : std::clamp(dexterity, 0, 75);
  const double delay = max_delay - (max_delay - min_delay) * dex / 75.0;
  // The wiki says "reduces by 33%"; whether that's x0.67 or /1.5 is open.
  return status.berserk && !status.dazed ? delay * 0.67 : delay;
}

double VolleysPerSecond(const Subattack& subattack, int dexterity, const PlayerStatus& status) {
  const double rate = AttacksPerSecond(dexterity, status) * subattack.rate_of_fire;
  if (!subattack.burst_count || !subattack.burst_delay) {
    return rate;
  }
  const double delay = BurstDelay(subattack, dexterity, status);
  if (delay <= 0) {
    return rate;
  }
  return std::min(rate, *subattack.burst_count / delay);
}

}  // namespace rotmg
