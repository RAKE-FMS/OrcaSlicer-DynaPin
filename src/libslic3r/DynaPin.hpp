#ifndef slic3r_DynaPin_hpp_
#define slic3r_DynaPin_hpp_

#include "ExPolygon.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class Print;
class PrintObject;

namespace DynaPin {

struct Pin
{
    int row = 0;
    int col = 0;
};

struct PullMoveConfig
{
    double x_hook = 0.;
    double x_latch = 0.;
    double x_front = 0.;
    double y_offset = 0.;
    double z_offset = 0.;
    double travel_feedrate = 6000.;
    double pull_feedrate = 1200.;
};

struct Config
{
    int    origin_row = 0;
    int    origin_col = 0;
    double origin_y = 0.;
    double origin_z = 0.;
    double row_pitch_y = 0.;
    double col_pitch_z = 0.;
    double blocker_center_x = 0.;
    double blocker_width_x = 0.;
    double blocker_width_y = 0.;
    double blocker_z_min = 0.;
    double blocker_z_max = 0.;
    PullMoveConfig pull_gcode;
};

// Axis-aligned 3D region (machine/world coordinates, mm) in which support
// material is excluded by a selected pin, together with the pin's own position.
// Used by the G-code preview to visualize pins and blocked regions.
struct BlockerBox
{
    Pin   pin;
    Vec3d min{Vec3d::Zero()};     // lower corner of the blocked region
    Vec3d max{Vec3d::Zero()};     // upper corner of the blocked region
    Vec3d pin_pos{Vec3d::Zero()}; // physical position of the pin
};

// A prism in which support material must be excluded. The polygon is expressed
// in the PrintObject's local (slice) coordinate system so it can be subtracted
// directly from generated support layers; [z_min, z_max] are print_z bounds (mm).
struct LocalBlocker
{
    Polygon poly;
    double  z_min = 0.;
    double  z_max = 0.;
};

std::vector<Pin> parse_pin_list(const std::string &pins);
bool load_config_for_print(const Print &print, Config &config, std::string *error = nullptr);
std::vector<Polygons> support_blockers_for_object(const PrintObject &object);
// Blocker prisms in object-local coordinates, used to clip already-generated
// support layers (e.g. support columns descending through the blocked region
// from overhangs located above it).
std::vector<LocalBlocker> support_blocker_regions_local(const PrintObject &object);
// Returns one box per selected pin (empty when DynaPin optimization is disabled
// or the config cannot be loaded).
std::vector<BlockerBox> selected_blocker_boxes(const Print &print);
std::string pull_gcode_for_pin(const Config &config, const Pin &pin);
double pin_y(const Config &config, const Pin &pin);
double pin_z(const Config &config, const Pin &pin);

} // namespace DynaPin
} // namespace Slic3r

#endif
