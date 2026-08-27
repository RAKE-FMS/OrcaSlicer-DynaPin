#include "DynaPin.hpp"

#include "Layer.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

namespace fs = boost::filesystem;

namespace Slic3r::DynaPin {

static double number_or(const nlohmann::json& j, const char* key, double fallback)
{
    if (!j.contains(key))
        return fallback;
    const nlohmann::json& value = j[key];
    if (value.is_number())
        return value.get<double>();
    if (value.is_string()) {
        try {
            return std::stod(value.get<std::string>());
        } catch (...) {}
    }
    return fallback;
}

static int int_or(const nlohmann::json& j, const char* key, int fallback)
{
    if (!j.contains(key))
        return fallback;
    const nlohmann::json& value = j[key];
    if (value.is_number_integer())
        return value.get<int>();
    if (value.is_string()) {
        try {
            return std::stoi(value.get<std::string>());
        } catch (...) {}
    }
    return fallback;
}

// The physical blocker/pin landing region is shifted along Y from the support
// origin. Pull G-code keeps its own origin and is intentionally independent of
// this support geometry.
static constexpr double support_block_y_offset = -7.2;

double pin_y(const Config& config, const Pin& pin)
{ return config.support_origin_y + double(pin.row) * config.row_pitch_y; }

BlockerZRange blocker_z_range(const Config& config, const Pin& pin)
{
    const double z_max = config.support_origin_z + double(pin.col) * config.col_pitch_z;
    return { z_max - config.blocker_height_z, z_max };
}

static double pull_y(const Config& config, const Pin& pin)
{ return config.pull_origin_y + double(pin.row) * config.row_pitch_y + config.pull_gcode.y_offset; }

static double pull_z(const Config& config, const Pin& pin)
{ return config.pull_origin_z + double(pin.col) * config.col_pitch_z; }

std::vector<Pin> parse_pin_list(const std::string& pins)
{
    std::vector<Pin>              out;
    std::set<std::pair<int, int>> seen;
    std::vector<std::string>      tokens;
    boost::split(tokens, pins, boost::is_any_of(";\n "), boost::token_compress_on);
    for (const std::string& token : tokens) {
        if (token.empty())
            continue;
        std::vector<std::string> pair;
        boost::split(pair, token, boost::is_any_of(","), boost::token_compress_on);
        if (pair.size() != 2)
            continue;
        try {
            Pin pin{std::stoi(pair[0]), std::stoi(pair[1])};
            if (seen.emplace(pin.row, pin.col).second)
                out.push_back(pin);
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "Ignoring invalid DynaPin pin token: " << token;
        }
    }
    return out;
}

bool has_manual_selection(const Print& print)
{
    const std::string& value = print.config().dynapin_selected_pins.value;
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); });
}

std::vector<Pin> candidate_pins(const Config& config)
{
    std::vector<Pin> out;
    if (config.row_count <= 0 || config.col_count <= 0)
        return out;
    out.reserve(size_t(config.row_count) * size_t(config.col_count));
    for (int row = 0; row < config.row_count; ++row)
        for (int col = 0; col < config.col_count; ++col)
            out.push_back({row, col});
    return out;
}

void sort_unique_pins(std::vector<Pin>& pins, const Config& config)
{
    std::sort(pins.begin(), pins.end(), [&config](const Pin& lhs, const Pin& rhs) {
        const double lhs_z = blocker_z_range(config, lhs).z_max;
        const double rhs_z = blocker_z_range(config, rhs).z_max;
        if (lhs_z != rhs_z)
            return lhs_z < rhs_z;
        if (lhs.row != rhs.row)
            return lhs.row < rhs.row;
        return lhs.col < rhs.col;
    });
    pins.erase(std::unique(pins.begin(), pins.end()), pins.end());
}

ProjectionSelection select_from_projection(std::vector<ProjectionEvent> events)
{
    auto type_order = [](ProjectionEventType type) {
        switch (type) {
        case ProjectionEventType::Contact:    return 0;
        case ProjectionEventType::PinSurface: return 1;
        case ProjectionEventType::Model:      return 2;
        }
        return 3;
    };
    std::stable_sort(events.begin(), events.end(), [&type_order](const ProjectionEvent& lhs, const ProjectionEvent& rhs) {
        if (lhs.print_z != rhs.print_z)
            return lhs.print_z > rhs.print_z;
        return type_order(lhs.type) < type_order(rhs.type);
    });

    ProjectionSelection result;
    Polygons             projection;
    for (const ProjectionEvent& event : events) {
        if (event.type == ProjectionEventType::Contact) {
            polygons_append(projection, event.polygons);
            projection = union_(projection);
        } else if (event.type == ProjectionEventType::Model) {
            if (!projection.empty())
                projection = diff(projection, offset(event.polygons, float(SCALED_EPSILON)));
        } else if (!projection.empty() && !intersection(projection, event.polygons).empty()) {
            if (event.colliding)
                result.rejected_collisions.push_back(event.pin);
            else {
                result.selected.push_back(event.pin);
                // This is the downward-propagation termination point. A safe
                // upper pin consumes only the projection landing on its top.
                projection = diff(projection, event.polygons);
            }
        }
    }
    return result;
}

const std::vector<Pin>& resolved_pins(const Print& print)
{
    return print.dynapin_selection().pins;
}

static std::vector<fs::path> config_candidates(const std::string& path)
{
    fs::path p(path);
    if (p.is_absolute())
        return {p};

    std::vector<fs::path> out;
    if (!data_dir().empty()) {
        out.emplace_back(fs::path(data_dir()) / "user" / p);
        out.emplace_back(fs::path(data_dir()) / "system" / p);
    }
    if (!resources_dir().empty()) {
        out.emplace_back(fs::path(resources_dir()) / "profiles" / p);
        out.emplace_back(fs::path(resources_dir()) / p);
    }
    out.emplace_back(p);
    return out;
}

static bool has_number_value(const nlohmann::json& object, const char* key)
{
    if (!object.contains(key))
        return false;
    const nlohmann::json& value = object[key];
    if (value.is_number())
        return true;
    if (!value.is_string())
        return false;

    try {
        size_t consumed = 0;
        std::stod(value.get<std::string>(), &consumed);
        return consumed == value.get<std::string>().size();
    } catch (...) {
        return false;
    }
}

static std::optional<double> bed_max_x(const Print& print)
{
    const Points bed = get_bed_shape(print.config());
    if (bed.empty())
        return std::nullopt;

    coord_t max_x = bed.front().x();
    for (const Point& point : bed)
        max_x = std::max(max_x, point.x());
    return unscale_(max_x);
}

bool load_config_for_print(const Print& print, Config& config, std::string* error)
{
    const std::string config_path = print.config().dynapin_config_path.value;
    if (config_path.empty()) {
        if (error)
            *error = "DynaPin config path is empty";
        return false;
    }

    fs::path resolved;
    for (const fs::path& candidate : config_candidates(config_path)) {
        if (fs::exists(candidate)) {
            resolved = candidate;
            break;
        }
    }
    if (resolved.empty()) {
        if (error)
            *error = "DynaPin config file not found: " + config_path;
        return false;
    }

    boost::nowide::ifstream ifs(resolved.string());
    if (!ifs) {
        if (error)
            *error = "Failed to open DynaPin config: " + resolved.string();
        return false;
    }

    nlohmann::json j = nlohmann::json::parse(ifs, nullptr, false);
    if (j.is_discarded()) {
        if (error)
            *error = "Invalid DynaPin JSON: " + resolved.string();
        return false;
    }

    const nlohmann::json grid           = j.contains("grid") && j["grid"].is_object() ? j["grid"] : nlohmann::json::object();
    config.row_count                    = int_or(grid, "row_count", config.row_count);
    config.col_count                    = int_or(grid, "col_count", config.col_count);
    const nlohmann::json pull_origin    = grid.contains("pull_origin") && grid["pull_origin"].is_object() ? grid["pull_origin"] :
                                                                                                            nlohmann::json::object();
    config.pull_origin_y                = number_or(pull_origin, "y", config.pull_origin_y);
    config.pull_origin_z                = number_or(pull_origin, "z", config.pull_origin_z);
    const nlohmann::json support_origin = grid.contains("support_origin") && grid["support_origin"].is_object() ? grid["support_origin"] :
                                                                                                                  nlohmann::json::object();
    config.support_origin_y             = number_or(support_origin, "y", config.support_origin_y);
    config.support_origin_z             = number_or(support_origin, "z", config.support_origin_z);
    const nlohmann::json pitch          = grid.contains("pitch") && grid["pitch"].is_object() ? grid["pitch"] : nlohmann::json::object();
    config.row_pitch_y                  = number_or(pitch, "row_y", config.row_pitch_y);
    config.col_pitch_z                  = number_or(pitch, "col_z", config.col_pitch_z);

    const nlohmann::json exclusion = j.contains("support_exclusion") && j["support_exclusion"].is_object() ? j["support_exclusion"] :
                                                                                                             nlohmann::json::object();
    config.blocker_width_y  = number_or(exclusion, "blocker_width_y", config.blocker_width_y);
    config.blocker_height_z = number_or(exclusion, "blocker_height_z", config.blocker_height_z);

    const nlohmann::json pull  = j.contains("pull_gcode") && j["pull_gcode"].is_object() ? j["pull_gcode"] : nlohmann::json::object();
    config.pull_gcode.x_hook   = number_or(pull, "x_hook", config.pull_gcode.x_hook);
    config.pull_gcode.x_latch  = number_or(pull, "x_latch", config.pull_gcode.x_latch);
    config.pull_gcode.x_front  = number_or(pull, "x_front", config.pull_gcode.x_front);
    config.pull_gcode.y_offset = number_or(pull, "y_offset", config.pull_gcode.y_offset);
    config.pull_gcode.z_offset = number_or(pull, "z_offset", config.pull_gcode.z_offset);
    config.pull_gcode.travel_feedrate    = number_or(pull, "travel_feedrate", config.pull_gcode.travel_feedrate);
    config.pull_gcode.pull_feedrate      = number_or(pull, "pull_feedrate", config.pull_gcode.pull_feedrate);
    config.pull_gcode.travel_feedrate    = number_or(pull, "fast_feed_rate", config.pull_gcode.travel_feedrate);
    config.pull_gcode.pull_feedrate      = number_or(pull, "feed_rate", config.pull_gcode.pull_feedrate);
    config.pull_gcode.approach_y_offset  = number_or(pull, "approach_y_offset", config.pull_gcode.approach_y_offset);
    config.pull_gcode.pull_feedrate_fast = number_or(pull, "pull_feedrate_fast", config.pull_gcode.pull_feedrate_fast);
    config.pull_gcode.disengage_x_offset = number_or(pull, "disengage_x_offset", config.pull_gcode.disengage_x_offset);

    const std::optional<double> max_x_bed = bed_max_x(print);
    if (!max_x_bed) {
        const std::string message = "DynaPin config requires a printable bed X range";
        if (error)
            *error = message;
        else
            BOOST_LOG_TRIVIAL(warning) << "[DynaPin] cannot generate geometry: " << message;
        return false;
    }
    if (!has_number_value(pull, "x_front") || !std::isfinite(config.pull_gcode.x_front) || config.pull_gcode.x_front >= *max_x_bed ||
        config.blocker_width_y <= 0. || config.row_pitch_y == 0. || config.col_pitch_z == 0.) {
        if (error)
            *error = "DynaPin config requires pull_gcode.x_front and valid grid/support_exclusion dimensions";
        return false;
    }

    const BlockerZRange zero_pin_range = blocker_z_range(config, { 0, 0 });
    BOOST_LOG_TRIVIAL(debug) << "[DynaPin] config loaded: requested='" << config_path << "', resolved='" << resolved.string()
                             << "', grid_size=" << config.row_count << "x" << config.col_count << ", pull_origin_mm=("
                             << config.pull_origin_y << "," << config.pull_origin_z << ")"
                             << ", support_origin_mm=(" << config.support_origin_y << "," << config.support_origin_z << ")"
                             << ", pitch_mm=(" << config.row_pitch_y << "," << config.col_pitch_z << ")"
                             << ", exclusion_x_mm=[" << config.pull_gcode.x_front << "," << *max_x_bed << "]"
                             << ", exclusion_y_center_mm=" << config.support_origin_y + support_block_y_offset
                             << ", exclusion_width_y_mm=" << config.blocker_width_y << ", zero_column_blocker_z_range_mm=["
                             << zero_pin_range.z_min << "," << zero_pin_range.z_max << "]";
    return true;
}

static LocalBlocker blocker_for_pin_shift(const Config& config, const Pin& pin, const Point& shift, double max_x_bed)
{
    const double y       = pin_y(config, pin);
    const BlockerZRange z_range = blocker_z_range(config, pin);
    const double block_y = y + support_block_y_offset;
    const double min_x   = config.pull_gcode.x_front;
    const double max_x   = max_x_bed;
    const double min_y   = block_y - 0.5 * config.blocker_width_y;
    const double max_y   = block_y + 0.5 * config.blocker_width_y;

    LocalBlocker blocker;
    blocker.z_min       = z_range.z_min;
    blocker.z_max       = z_range.z_max;
    blocker.poly.points = {Point(scale_(min_x) - shift.x(), scale_(min_y) - shift.y()),
                           Point(scale_(max_x) - shift.x(), scale_(min_y) - shift.y()),
                           Point(scale_(max_x) - shift.x(), scale_(max_y) - shift.y()),
                           Point(scale_(min_x) - shift.x(), scale_(max_y) - shift.y())};
    return blocker;
}

LocalBlocker blocker_for_pin(const PrintObject& object, const Config& config, const Pin& pin)
{
    const Print& print = *object.print();
    const Point shift = object.instances().empty() ? Point(0, 0) : object.instances().front().shift_without_plate_offset();
    const std::optional<double> max_x_bed = bed_max_x(print);
    if (!max_x_bed)
        return {};
    return blocker_for_pin_shift(config, pin, shift, *max_x_bed);
}

static VirtualSupportSurface surface_for_pin_shift(const Config& config, const Pin& pin, const Point& shift, double max_x_bed)
{
    const double block_y = pin_y(config, pin) + support_block_y_offset;
    const double min_x   = config.pull_gcode.x_front;
    const double max_x   = max_x_bed;
    const double min_y   = block_y - 0.5 * config.blocker_width_y;
    const double max_y   = block_y + 0.5 * config.blocker_width_y;

    VirtualSupportSurface surface;
    surface.print_z     = blocker_z_range(config, pin).z_max;
    surface.poly.points = {Point(scale_(min_x) - shift.x(), scale_(min_y) - shift.y()),
                           Point(scale_(max_x) - shift.x(), scale_(min_y) - shift.y()),
                           Point(scale_(max_x) - shift.x(), scale_(max_y) - shift.y()),
                           Point(scale_(min_x) - shift.x(), scale_(max_y) - shift.y())};
    return surface;
}

VirtualSupportSurface surface_for_pin(const PrintObject& object, const Config& config, const Pin& pin)
{
    const Print& print = *object.print();
    const Point shift  = object.instances().empty() ? Point(0, 0) : object.instances().front().shift_without_plate_offset();
    const std::optional<double> max_x_bed = bed_max_x(print);
    if (!max_x_bed)
        return {};
    return surface_for_pin_shift(config, pin, shift, *max_x_bed);
}

bool pin_collides_with_model(const Print& print, const Config& config, const Pin& pin)
{
    const std::optional<double> max_x_bed = bed_max_x(print);
    if (!max_x_bed) {
        BOOST_LOG_TRIVIAL(warning) << "[DynaPin] cannot check pin collision without a printable bed X range";
        return true;
    }
    for (const PrintObject* object : print.objects()) {
        const auto check_instance = [&](const Point& shift) {
            const LocalBlocker blocker = blocker_for_pin_shift(config, pin, shift, *max_x_bed);
            for (const Layer* layer : object->layers()) {
                if (layer->print_z + EPSILON < blocker.z_min || layer->print_z - EPSILON > blocker.z_max)
                    continue;
                const Polygons overlap = intersection(to_polygons(layer->lslices), {blocker.poly});
                if (!overlap.empty()) {
                    BOOST_LOG_TRIVIAL(debug) << "[DynaPin] model collision: pin=(" << pin.row << "," << pin.col << ")"
                                             << ", layer_id=" << layer->id() << ", layer_z=" << layer->print_z
                                             << ", instance_shift_mm=(" << unscale<double>(shift.x()) << ","
                                             << unscale<double>(shift.y()) << ")"
                                             << ", blocker_x_mm=[" << unscale<double>(blocker.poly.points.front().x() + shift.x())
                                             << "," << unscale<double>(blocker.poly.points[1].x() + shift.x()) << "]"
                                             << ", overlap_area_mm2=" << area(overlap) * SCALING_FACTOR * SCALING_FACTOR;
                    return true;
                }
            }
            return false;
        };

        if (object->instances().empty()) {
            if (check_instance(Point(0, 0)))
                return true;
        } else {
            for (const PrintInstance &instance : object->instances()) {
                if (check_instance(instance.shift_without_plate_offset()))
                    return true;
            }
        }
    }
    return false;
}

std::vector<Polygons> support_blockers_for_object(const PrintObject& object)
{
    std::vector<Polygons> out(object.layer_count());
    const Print&          print = *object.print();
    if (!print.config().enable_dynapin_support_optimization.value)
        return out;

    Config      config;
    std::string error;
    if (!load_config_for_print(print, config, &error)) {
        BOOST_LOG_TRIVIAL(warning) << error;
        return out;
    }

    const std::vector<Pin>& pins = resolved_pins(print);
    // The blocker geometry above is expressed in world (machine/bed) coordinates,
    // matching the G-code preview overlay. The support polygons we subtract it from
    // (layer.lslices etc.) live in object-local coordinates: world = local + instance.shift.
    // Convert the blocker into object-local space by subtracting the instance shift,
    // otherwise the blocker lands in the wrong place and support is still generated
    // inside the previewed block region. Use shift_without_plate_offset() (the same
    // transform PrintObject uses to bring world polygons into object coords, see
    // PrintObject.cpp), because instance.shift also carries the large multi-plate
    // offset which would push the blocker far off the bed.
    for (const Pin& pin : pins) {
        const LocalBlocker blocker = blocker_for_pin(object, config, pin);
        for (const Layer* layer : object.layers()) {
            if (layer->print_z + EPSILON >= blocker.z_min && layer->print_z - EPSILON <= blocker.z_max)
                out[layer->id()].push_back(blocker.poly);
        }
    }
    return out;
}

std::vector<LocalBlocker> support_blocker_regions_local(const PrintObject& object)
{
    std::vector<LocalBlocker> out;
    const Print&              print = *object.print();
    if (!print.config().enable_dynapin_support_optimization.value)
        return out;

    Config      config;
    std::string error;
    if (!load_config_for_print(print, config, &error)) {
        BOOST_LOG_TRIVIAL(warning) << error;
        return out;
    }

    const std::vector<Pin>& pins     = resolved_pins(print);
    out.reserve(pins.size());
    for (const Pin& pin : pins)
        out.push_back(blocker_for_pin(object, config, pin));
    return out;
}

std::vector<VirtualSupportSurface> pin_top_surfaces_for_object(const PrintObject& object)
{
    std::vector<VirtualSupportSurface> out;
    const Print&                       print = *object.print();
    if (!print.config().enable_dynapin_support_optimization.value)
        return out;

    Config      config;
    std::string error;
    if (!load_config_for_print(print, config, &error)) {
        BOOST_LOG_TRIVIAL(warning) << error;
        return out;
    }

    const std::vector<Pin>& pins     = resolved_pins(print);
    out.reserve(pins.size());
    for (const Pin& pin : pins)
        out.push_back(surface_for_pin(object, config, pin));
    return out;
}

std::vector<BlockerBox> selected_blocker_boxes(const Print& print)
{
    std::vector<BlockerBox> out;
    if (!print.config().enable_dynapin_support_optimization.value)
        return out;

    Config      config;
    std::string error;
    if (!load_config_for_print(print, config, &error)) {
        BOOST_LOG_TRIVIAL(warning) << error;
        return out;
    }

    const std::vector<Pin>& pins = resolved_pins(print);
    const std::optional<double> max_x_bed = bed_max_x(print);
    if (!max_x_bed)
        return out;
    out.reserve(pins.size());
    for (const Pin& pin : pins) {
        const double y       = pin_y(config, pin);
        const BlockerZRange z_range = blocker_z_range(config, pin);
        const double block_y = y + support_block_y_offset;
        const double min_x   = config.pull_gcode.x_front;
        const double max_x   = *max_x_bed;
        const double min_y   = block_y - 0.5 * config.blocker_width_y;
        const double max_y   = block_y + 0.5 * config.blocker_width_y;
        BlockerBox box;
        box.pin     = pin;
        box.min     = Vec3d(min_x, min_y, z_range.z_min);
        box.max     = Vec3d(max_x, max_y, z_range.z_max);
        box.pin_pos = Vec3d(min_x, block_y, z_range.z_max);
        out.push_back(box);
    }
    return out;
}

std::string pull_gcode_for_pin(const Config& config, const Pin& pin)
{
    const double       y      = pull_y(config, pin);
    const double       z      = pull_z(config, pin);
    const double       z_ret  = z + config.pull_gcode.z_offset;
    const double       y_app  = y + config.pull_gcode.approach_y_offset;
    const double       f_fast = config.pull_gcode.travel_feedrate;
    const double       f_slow = config.pull_gcode.pull_feedrate;
    const double       f_pull = config.pull_gcode.pull_feedrate_fast;
    std::ostringstream gcode;
    gcode << std::fixed << std::setprecision(4);
    gcode << "; BEGIN_DYNAPIN_PULL ROW=" << pin.row << " COL=" << pin.col << "\n";
    // Approach: shift Y away from pin, then move X+Z together to hook height
    gcode << "G1 Y" << y_app << " F" << f_fast << "\n";
    gcode << "G1 X" << config.pull_gcode.x_hook << " Z" << z << " F" << f_fast << "\n";
    // Latch: advance X to latch position
    gcode << "G1 X" << config.pull_gcode.x_latch << " F" << f_slow << "\n";
    // Engage: move Y back onto pin
    gcode << "G1 Y" << y << " F" << f_slow << "\n";
    gcode << "; DYNAPIN_PULL_MOVE\n";
    // Pull pin out fast
    gcode << "G1 X" << config.pull_gcode.x_front << " F" << f_pull << "\n";
    // Disengage: shift X slightly and Y away
    gcode << "G1 X" << (config.pull_gcode.x_front + config.pull_gcode.disengage_x_offset) << " Y" << y_app << " F" << f_slow << "\n";
    // Retract Z to clear
    gcode << "G1 Z" << z_ret << " F" << f_slow << "\n";
    gcode << "; END_DYNAPIN_PULL\n";
    return gcode.str();
}

} // namespace Slic3r::DynaPin
