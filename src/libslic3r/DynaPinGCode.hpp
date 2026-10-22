#ifndef slic3r_DynaPinGCode_hpp_
#define slic3r_DynaPinGCode_hpp_

#include "DynaPin.hpp"

namespace Slic3r::DynaPin {

// Keep directional support speeds intact while retaining ordinary cooling adjustment.
inline constexpr const char* SUPPORT_FIXED_SPEED = ";_DYNAPIN_SUPPORT_FIXED_SPEED";

std::string return_gcode(const Config& config, const Vec3d& return_position);

} // namespace Slic3r::DynaPin

#endif
