#pragma once

#include "rotmg/damage.hpp"
#include "rotmg/model.hpp"

// How often a weapon fires. Rules come from the RealmEye wiki pages Character
// Stats, Status Effects, Weapon Attributes and Longbows.
namespace rotmg {

/// Attacks per second before the weapon's rate of fire: 1.5 + 6.5 * DEX / 75.
/// Berserk adds 25%. Dazed sets DEX to 0 and cancels Berserk.
double AttacksPerSecond(int dexterity, const PlayerStatus& status);

/// Seconds from the start of one burst to the start of the next. DEX moves it
/// from the max delay at 0 DEX to the min delay at 75, and no further. Berserk
/// takes 33% off. Only meaningful when the subattack has a burst.
double BurstDelay(const Subattack& subattack, int dexterity, const PlayerStatus& status);

/// Volleys per second for one subattack: attacks per second times its rate of
/// fire, capped at burst count / burst delay for burst weapons.
double VolleysPerSecond(const Subattack& subattack, int dexterity, const PlayerStatus& status);

}  // namespace rotmg
