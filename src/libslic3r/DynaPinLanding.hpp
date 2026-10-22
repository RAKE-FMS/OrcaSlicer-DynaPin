#ifndef slic3r_DynaPinLanding_hpp_
#define slic3r_DynaPinLanding_hpp_

#include "ExtrusionEntityCollection.hpp"
#include "Flow.hpp"

namespace Slic3r {
class PrintObject;
class Print;
class SupportLayer;
}

namespace Slic3r::DynaPin {
struct Config;
struct Pin;
// Return zero when the nozzle can traverse at target_z. Only an intersected
// pin surface at or above that height requires the configured extra clearance.
// local_path + machine_origin gives the physical nozzle XY on this plate.
double required_pin_travel_z(const Print&            print,
                             const Config&           config,
                             const std::vector<Pin>& pulled_pins,
                             const Polyline&         local_path,
                             const Vec2d&            machine_origin,
                             double                  target_z,
                             double                  nozzle_radius);

// Keep paths flat until support height modulation has finished.
void fill_landing_paths(
    ExtrusionEntitiesPtr& out, const Polygons& regions, const Flow& flow, double density, coord_t origin_x, bool fit_edges = false);
// Assemble all source slabs into one non-reversible collection per connected region.
void order_landing_paths(ExtrusionEntityCollection& collection, const Polygons& regions);
// Source identifies the owning object instance; paths use machine XY coordinates.
struct LandingExtrusion
{
    size_t        source;
    ExtrusionPath path;
};
struct LandingRegion
{
    size_t   source;
    float    height;
    Polygons polygons;
};
std::vector<LandingRegion> landing_extrusion_regions(const SupportLayer& layer, const Point& instance_shift, size_t source);
// Read-only preparation for collision checking, using the same support trimming
// and shared landing raster as export. Paths are in machine XY coordinates.
struct SupportExtrusions
{
    const PrintObject*  object;
    const SupportLayer* layer;
    ExtrusionPaths      paths;
};
std::vector<SupportExtrusions> normalized_support_paths(const std::vector<PrintObject*>& objects);
bool                           starts_on_pin(const PrintObject& object);
// Process one instance's support at one Z and extruder. Keep ownership, flow,
// transverse crossings and distinct parallel lines; trim only collinear repeats
// from earlier instances. Source collections remain reusable for other copies.
void remove_duplicate_support_paths(ExtrusionEntityCollection&     collection,
                                    const Point&                   instance_shift,
                                    size_t                         source,
                                    ExtrusionRole                  role,
                                    std::vector<LandingExtrusion>& emitted);
void collect_landing_paths(const ExtrusionEntityCollection& collection, ExtrusionPaths& out);
// Shared multi-model output should omit source-local links so they cannot skip global scan columns.
// With source densities, overlapping equal-flow landing footprints are refilled
// on a common machine grid before ordering. Other flow groups remain distinct.
std::vector<LandingExtrusion> schedule_landing_paths(std::vector<LandingExtrusion>     paths,
                                                     bool                              include_source_links = true,
                                                     const std::vector<double>&        source_densities     = {},
                                                     const std::vector<LandingRegion>& source_regions       = {});
// Use the final extrusion heights: a SupportLayer may merge slabs with different bottoms.
std::vector<Polygons> support_stabilization_regions(const PrintObject& object, size_t layer_count);
// Limit the X component without accelerating already slower extrusion.
double support_segment_speed(const Line& line, double speed, double max_x_speed);
bool   ordered_landing_collection(const ExtrusionEntityCollection& collection);
} // namespace Slic3r::DynaPin

#endif
