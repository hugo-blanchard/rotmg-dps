#pragma once

#include <string>
#include <vector>

#include "rotmg/model.hpp"

// Geometric hit estimate: how many projectiles of a volley pass through a
// stationary round target when the player aims straight at its center.
// Motion rules come from the RealmEye wiki page Weapon Attributes.
namespace rotmg {

/// The default target radius in tiles. With it, a tiered bow's side shots
/// stop hitting at about 3.56 tiles, the wiki's "true range".
inline constexpr double kDefaultTargetRadius = 0.5;

/// Reasons the estimate can't model this weapon's movement, in plain words.
/// Empty when it can. Callers fall back to assuming every projectile hits.
std::vector<std::string> HitModelGaps(const Weapon& weapon);

/// Whether HitModelGaps is empty, without building it.
bool HitModelFits(const Weapon& weapon);

/// Projectiles of one volley of `subattack` that hit a target of `radius`
/// tiles centered `distance` tiles away. Each projectile hits a target at
/// most once.
int HitsPerVolley(const Weapon& weapon, const Subattack& subattack, double distance, double radius);

/// Rules the estimate relies on that have no source, for the weapon's
/// projectiles. Listed so the UI can show them.
std::vector<std::string> HitModelAssumptions(const Weapon& weapon);

/// Of `distances` (in tiles), the ones worth scoring a build with this
/// weapon and ability at: the farthest that lands each different set of
/// hits per volley over the weapon's and the ability's subattacks, leaving
/// out sets another one matches or beats on every subattack. Distance only
/// changes how many shots land and more never deal less, so the best score
/// over these is the best over all of `distances`. Ascending.
std::vector<double> DistancesWorthScoring(const Weapon& weapon, const Ability* ability,
                                          const std::vector<double>& distances, double radius);

}  // namespace rotmg
