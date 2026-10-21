#ifndef slic3r_DynaPinLanding_hpp_
#define slic3r_DynaPinLanding_hpp_

#include "ExtrusionEntityCollection.hpp"
#include "Flow.hpp"

namespace Slic3r::DynaPin {
// Keep paths flat until support height modulation has finished.
void fill_landing_paths(ExtrusionEntitiesPtr& out, const Polygons& regions, const Flow& flow, double density, coord_t origin_x);
// Assemble all source slabs into one non-reversible collection per connected region.
void order_landing_paths(ExtrusionEntityCollection& collection, const Polygons& regions);
bool ordered_landing_collection(const ExtrusionEntityCollection& collection);
} // namespace Slic3r::DynaPin

#endif
