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

std::vector<Pin> parse_pin_list(const std::string &pins);
bool load_config_for_print(const Print &print, Config &config, std::string *error = nullptr);
std::vector<Polygons> support_blockers_for_object(const PrintObject &object);
std::string pull_gcode_for_pin(const Config &config, const Pin &pin);
double pin_y(const Config &config, const Pin &pin);
double pin_z(const Config &config, const Pin &pin);

} // namespace DynaPin
} // namespace Slic3r

#endif
