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
#include <cmath>
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

double pin_y(const Config& config, const Pin& pin) { return config.origin_y + double(pin.row - config.origin_row) * config.row_pitch_y; }

double pin_z(const Config& config, const Pin& pin) { return config.origin_z + double(pin.col - config.origin_col) * config.col_pitch_z; }

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

    const nlohmann::json grid   = j.contains("grid") && j["grid"].is_object() ? j["grid"] : nlohmann::json::object();
    config.origin_row           = int_or(grid, "origin_row", config.origin_row);
    config.origin_col           = int_or(grid, "origin_col", config.origin_col);
    config.origin_y             = number_or(grid, "origin_y", config.origin_y);
    config.origin_z             = number_or(grid, "origin_z", config.origin_z);
    config.row_pitch_y          = number_or(grid, "row_pitch_y", config.row_pitch_y);
    config.col_pitch_z          = number_or(grid, "col_pitch_z", config.col_pitch_z);
    const nlohmann::json origin = grid.contains("origin") && grid["origin"].is_object() ? grid["origin"] : nlohmann::json::object();
    config.origin_row           = int_or(origin, "row", config.origin_row);
    config.origin_col           = int_or(origin, "col", config.origin_col);
    config.origin_y             = number_or(origin, "y", config.origin_y);
    config.origin_z             = number_or(origin, "z", config.origin_z);
    const nlohmann::json pitch  = grid.contains("pitch") && grid["pitch"].is_object() ? grid["pitch"] : nlohmann::json::object();
    config.row_pitch_y          = number_or(pitch, "row_y", config.row_pitch_y);
    config.col_pitch_z          = number_or(pitch, "col_z", config.col_pitch_z);

    const nlohmann::json exclusion = j.contains("support_exclusion") && j["support_exclusion"].is_object() ? j["support_exclusion"] :
                                                                                                             nlohmann::json::object();
    config.blocker_center_x        = number_or(exclusion, "center_x", number_or(exclusion, "x_center", config.blocker_center_x));
    config.blocker_width_x         = number_or(exclusion, "width_x", number_or(exclusion, "x_width", config.blocker_width_x));
    config.blocker_width_y         = number_or(exclusion, "width_y", number_or(exclusion, "y_width", config.blocker_width_y));
    if (exclusion.contains("x_min") && exclusion.contains("x_max") && exclusion["x_min"].is_number() && exclusion["x_max"].is_number()) {
        const double x_min      = exclusion["x_min"].get<double>();
        const double x_max      = exclusion["x_max"].get<double>();
        config.blocker_center_x = 0.5 * (x_min + x_max);
        config.blocker_width_x  = std::abs(x_max - x_min);
    }
    config.blocker_z_max = number_or(exclusion, "z_above", number_or(exclusion, "z_max", config.blocker_z_max));
    const double z_range = number_or(exclusion, "z_range", 0.);
    if (z_range > 0. && config.blocker_z_max == 0.)
        config.blocker_z_max = 0.5 * z_range;

    const nlohmann::json pull  = j.contains("pull_gcode") && j["pull_gcode"].is_object() ? j["pull_gcode"] : nlohmann::json::object();
    config.pull_gcode.x_hook   = number_or(pull, "x_hook", config.pull_gcode.x_hook);
    config.pull_gcode.x_latch  = number_or(pull, "x_latch", config.pull_gcode.x_latch);
    config.pull_gcode.x_front  = number_or(pull, "x_front", config.pull_gcode.x_front);
    config.pull_gcode.y_offset = number_or(pull, "y_offset", config.pull_gcode.y_offset);
    config.pull_gcode.z_offset = number_or(pull, "z_offset", config.pull_gcode.z_offset);
    config.pull_gcode.travel_feedrate = number_or(pull, "travel_feedrate", config.pull_gcode.travel_feedrate);
    config.pull_gcode.pull_feedrate   = number_or(pull, "pull_feedrate", config.pull_gcode.pull_feedrate);
    config.pull_gcode.travel_feedrate = number_or(pull, "fast_feed_rate", config.pull_gcode.travel_feedrate);
    config.pull_gcode.pull_feedrate   = number_or(pull, "feed_rate", config.pull_gcode.pull_feedrate);

    if (config.blocker_width_x <= 0. || config.blocker_width_y <= 0. || config.row_pitch_y == 0. || config.col_pitch_z == 0.) {
        if (error)
            *error = "DynaPin config is missing required grid or support_exclusion dimensions";
        return false;
    }
    return true;
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

    const std::vector<Pin> pins = parse_pin_list(print.config().dynapin_selected_pins.value);
    for (const Pin& pin : pins) {
        const double y     = pin_y(config, pin);
        const double z     = pin_z(config, pin);
        const double z_min = 0.;
        const double z_max = z + config.blocker_z_max;
        const double min_x = std::min(config.blocker_center_x - 0.5 * config.blocker_width_x, config.pull_gcode.x_front);
        const double max_x = config.blocker_center_x + 0.5 * config.blocker_width_x;
        const double min_y = y - 0.5 * config.blocker_width_y;
        const double max_y = y + 0.5 * config.blocker_width_y;

        Polygon poly;
        poly.points = {Point(scale_(min_x), scale_(min_y)), Point(scale_(max_x), scale_(min_y)), Point(scale_(max_x), scale_(max_y)),
                       Point(scale_(min_x), scale_(max_y))};
        for (const Layer* layer : object.layers()) {
            if (layer->print_z + EPSILON >= z_min && layer->print_z - EPSILON <= z_max)
                out[layer->id()].push_back(poly);
        }
    }
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

    const std::vector<Pin> pins = parse_pin_list(print.config().dynapin_selected_pins.value);
    out.reserve(pins.size());
    for (const Pin& pin : pins) {
        const double y     = pin_y(config, pin);
        const double z     = pin_z(config, pin);
        const double min_x = std::min(config.blocker_center_x - 0.5 * config.blocker_width_x, config.pull_gcode.x_front);
        const double max_x = config.blocker_center_x + 0.5 * config.blocker_width_x;
        const double min_y = y - 0.5 * config.blocker_width_y;
        const double max_y = y + 0.5 * config.blocker_width_y;
        const double z_min = 0.;
        const double z_max = z + config.blocker_z_max;

        BlockerBox box;
        box.pin     = pin;
        box.min     = Vec3d(min_x, min_y, z_min);
        box.max     = Vec3d(max_x, max_y, z_max);
        box.pin_pos = Vec3d(config.blocker_center_x, y, z);
        out.push_back(box);
    }
    return out;
}

std::string pull_gcode_for_pin(const Config& config, const Pin& pin)
{
    const double       y = pin_y(config, pin) + config.pull_gcode.y_offset;
    const double       z = pin_z(config, pin) + config.pull_gcode.z_offset;
    std::ostringstream gcode;
    gcode << "; BEGIN_DYNAPIN_PULL ROW=" << pin.row << " COL=" << pin.col << "\n";
    gcode << "G0 Z" << z << " F" << config.pull_gcode.travel_feedrate << "\n";
    gcode << "G0 X" << config.pull_gcode.x_hook << " Y" << y << " F" << config.pull_gcode.travel_feedrate << "\n";
    gcode << "G1 X" << config.pull_gcode.x_latch << " F" << config.pull_gcode.pull_feedrate << "\n";
    gcode << "; DYNAPIN_PULL_MOVE\n";
    gcode << "G1 X" << config.pull_gcode.x_front << " F" << config.pull_gcode.pull_feedrate << "\n";
    gcode << "; END_DYNAPIN_PULL\n";
    return gcode.str();
}

} // namespace Slic3r::DynaPin
