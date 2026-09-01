#ifndef slic3r_DynaPinGCode_hpp_
#define slic3r_DynaPinGCode_hpp_

#include "DynaPin.hpp"

namespace Slic3r::DynaPin {

std::string return_gcode(const Config& config, const Vec3d& return_position);

} // namespace Slic3r::DynaPin

#endif
