#ifndef slic3r_DynaPinSupport_hpp_
#define slic3r_DynaPinSupport_hpp_

#include "DynaPin.hpp"

namespace Slic3r::DynaPin {
// Printable footprints, in surface order, including the shared-edge inset rule.
std::vector<Polygons> landing_footprints(const std::vector<VirtualSupportSurface>& surfaces);
// Targets are actual supported overhangs, in layer order (not expanded interfaces).
// Empty targets on an otherwise terminal branch conservatively preserve it.
std::vector<Polygons> independent_support_removals(const std::vector<LandingLayer>&          layers,
                                                   const std::vector<VirtualSupportSurface>& surfaces,
                                                   const std::vector<Polygons>&              targets);

// Native support infill insets centerlines by half the spacing, not half the
// width. Margins supply the remaining distance required by each source flow.
std::vector<Polygons> independent_support_removals(const std::vector<LandingLayer>&          layers,
                                                   const std::vector<VirtualSupportSurface>& surfaces,
                                                   const std::vector<Polygons>&              targets,
                                                   const std::vector<double>&                extrusion_margins);
} // namespace Slic3r::DynaPin
#endif
