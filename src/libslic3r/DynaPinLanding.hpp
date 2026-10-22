#ifndef slic3r_DynaPinLanding_hpp_
#define slic3r_DynaPinLanding_hpp_

#include "ExtrusionEntityCollection.hpp"
#include "Flow.hpp"

namespace Slic3r {
class PrintObject;
}

namespace Slic3r::DynaPin {
// Keep paths flat until support height modulation has finished.
void fill_landing_paths(ExtrusionEntitiesPtr& out, const Polygons& regions, const Flow& flow, double density, coord_t origin_x);
// Assemble all source slabs into one non-reversible collection per connected region.
void order_landing_paths(ExtrusionEntityCollection& collection, const Polygons& regions);
// Source identifies the owning object instance; paths use machine XY coordinates.
struct LandingExtrusion
{
    size_t        source;
    ExtrusionPath path;
};
void collect_landing_paths(const ExtrusionEntityCollection& collection, ExtrusionPaths& out);
// Shared multi-model output should omit source-local links so they cannot skip global scan columns.
std::vector<LandingExtrusion> schedule_landing_paths(std::vector<LandingExtrusion> paths, bool include_source_links = true);
// Use the final extrusion heights: a SupportLayer may merge slabs with different bottoms.
std::vector<Polygons> support_stabilization_regions(const PrintObject& object, size_t layer_count);
// Limit the X component without accelerating already slower extrusion.
double support_segment_speed(const Line& line, double speed, double min_x_distance, double max_x_speed);
bool   ordered_landing_collection(const ExtrusionEntityCollection& collection);
} // namespace Slic3r::DynaPin

#endif
