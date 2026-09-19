#include "SliceStatisticsLog.hpp"

#include "ExtrusionEntity.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "Print.hpp"
#include "Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

namespace Slic3r {
namespace {

// Keep these conversions in sync with GizmoObjectManipulation and GCodeViewer.
constexpr double INCH_TO_MM = 25.4;
constexpr double OUNCE_TO_G = 28.34952;

std::string fixed_2(double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    return buffer;
}

std::string time_display(float seconds) { return short_time(get_time_dhms(seconds)); }

std::string percent_display(float fraction)
{
    if (fraction == 0.0f)
        return "0";
    if (fraction <= 0.001f)
        return "<0.1";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f", fraction * 100.0f);
    return buffer;
}

nlohmann::ordered_json usage(double volume_mm3, double diameter_mm, double density_g_cm3, bool imperial_units)
{
    const double length_mm            = diameter_mm > 0.0 ? volume_mm3 / (PI * std::pow(0.5 * diameter_mm, 2)) : 0.0;
    const double weight_g             = volume_mm3 * density_g_cm3 * 0.001;
    const double length_display_value = imperial_units ? length_mm / INCH_TO_MM : length_mm / 1000.0;
    const double weight_display_value = imperial_units ? weight_g / OUNCE_TO_G : weight_g;
    const char*  length_unit          = imperial_units ? "in" : "m";
    const char*  weight_unit          = imperial_units ? "oz" : "g";
    return {{"length_display", fixed_2(length_display_value) + length_unit},
            {"weight_display", fixed_2(weight_display_value) + weight_unit},
            {"length_mm", length_mm},
            {"weight_g", weight_g},
            {"volume_mm3", volume_mm3}};
}

double volume_for(const std::map<size_t, double>& volumes, size_t filament_id)
{
    auto it = volumes.find(filament_id);
    return it == volumes.end() ? 0.0 : it->second;
}

} // namespace

bool should_log_slice_statistics(bool slice_succeeded, bool is_fff, const GCodeProcessorResult* result, unsigned int last_logged_result_id)
{ return slice_succeeded && is_fff && result != nullptr && result->id != last_logged_result_id; }

nlohmann::ordered_json make_slice_statistics_log(const GCodeProcessorResult& result,
                                         const PrintStatistics&      print_statistics,
                                         int                         plate_index,
                                         bool                        imperial_units)
{
    const PrintEstimatedStatistics& estimated = result.print_statistics;
    // Match the preview's line types, filament table, and summary before auxiliary statistics.
    nlohmann::ordered_json record = {{"plate_index", plate_index},
                                     {"result_id", result.id},
                                     {"unit_system", imperial_units ? "imperial" : "metric"},
                                     {"roles", nlohmann::ordered_json::array()},
                                     {"filaments", nlohmann::ordered_json::array()},
                                     {"total_filament", nlohmann::ordered_json::object()},
                                     {"model_filament", nlohmann::ordered_json::object()},
                                     {"cost", {{"display", fixed_2(print_statistics.total_cost)}, {"raw", print_statistics.total_cost}}},
                                     {"time", nlohmann::ordered_json::object()},
                                     {"filament_changes", estimated.total_filament_changes},
                                     {"details", {{"extruder_changes", estimated.total_extruder_changes},
                                                  {"toolchanges", print_statistics.total_toolchanges},
                                                  {"total_extruded_volume_mm3", print_statistics.total_extruded_volume},
                                                  {"wipe_tower_filament_mm", print_statistics.total_wipe_tower_filament},
                                                  {"wipe_tower_cost", print_statistics.total_wipe_tower_cost},
                                                  {"estimated_normal_print_time", print_statistics.estimated_normal_print_time},
                                                  {"estimated_silent_print_time", print_statistics.estimated_silent_print_time},
                                                  {"initial_tool", print_statistics.initial_tool},
                                                  {"filament_stats_volume_mm3", nlohmann::ordered_json::array()},
                                                  {"volumes_per_color_change_mm3", estimated.volumes_per_color_change}}}};
    for (const auto& [filament_id, volume] : print_statistics.filament_stats)
        record["details"]["filament_stats_volume_mm3"].push_back({{"filament_id", filament_id + 1}, {"volume_mm3", volume}});

    const double length_divisor = imperial_units ? INCH_TO_MM : 1000.0;
    const double weight_divisor = imperial_units ? OUNCE_TO_G : 1.0;
    const char*  length_unit    = imperial_units ? "in" : "m";
    const char*  weight_unit    = imperial_units ? "oz" : "g";
    record["total_filament"]    = {{"length_display", fixed_2(print_statistics.total_used_filament / length_divisor) + length_unit},
                                   {"weight_display", fixed_2(print_statistics.total_weight / weight_divisor) + weight_unit},
                                   {"length_mm", print_statistics.total_used_filament},
                                   {"weight_g", print_statistics.total_weight}};

    std::array<std::map<ExtrusionRole, float>, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)> role_times;
    std::array<float, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)>                          travel_times{0.0f, 0.0f};
    // libvgcode converts moves beginning at index 1; its inserted phantom vertices have zero time.
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& move = result.moves[i];
        for (size_t mode = 0; mode < travel_times.size(); ++mode) {
            if (move.type == EMoveType::Extrude)
                role_times[mode][move.extrusion_role] += move.time[mode];
            else if (move.type == EMoveType::Travel)
                travel_times[mode] += move.time[mode];
        }
    }

    const char* mode_names[] = {"normal", "stealth"};
    for (size_t mode = 0; mode < travel_times.size(); ++mode) {
        const auto& data                 = estimated.modes[mode];
        record["time"][mode_names[mode]] = {{"prepare_display", time_display(data.prepare_time)},
                                            {"prepare_seconds", data.prepare_time},
                                            {"model_display", time_display(data.time - data.prepare_time)},
                                            {"model_seconds", data.time - data.prepare_time},
                                            {"total_display", time_display(data.time)},
                                            {"total_seconds", data.time},
                                            {"travel", nlohmann::ordered_json::object()},
                                            {"custom_gcode_times", nlohmann::ordered_json::array()}};
        for (const auto& entry : data.custom_gcode_times)
            record["time"][mode_names[mode]]["custom_gcode_times"].push_back({{"type", static_cast<int>(entry.first)},
                                                                              {"duration_seconds", entry.second.first},
                                                                              {"remaining_seconds", entry.second.second}});

        const float travel_fraction                = data.time > 0.0f ? travel_times[mode] / data.time : 0.0f;
        record["time"][mode_names[mode]]["travel"] = {{"display", travel_times[mode] > 0.0f ? time_display(travel_times[mode]) : ""},
                                                      {"percent_display", percent_display(travel_fraction)},
                                                      {"seconds", travel_times[mode]},
                                                      {"percent", travel_fraction * 100.0f}};
    }

    std::set<ExtrusionRole> roles;
    for (const auto& mode : role_times)
        for (const auto& [role, time] : mode)
            roles.insert(role);
    for (const auto& [role, filament] : estimated.used_filaments_per_role)
        roles.insert(role);
    for (ExtrusionRole role : roles) {
        auto           filament = estimated.used_filaments_per_role.find(role);
        const double   length_m = filament == estimated.used_filaments_per_role.end() ? 0.0 : filament->second.first;
        const double   weight_g = filament == estimated.used_filaments_per_role.end() ? 0.0 : filament->second.second;
        nlohmann::ordered_json item = {{"role", ExtrusionEntity::role_to_string(role)},
                                       {"time", nlohmann::ordered_json::object()},
                                       {"length_display", fixed_2(length_m * 1000.0 / length_divisor) + length_unit},
                                       {"weight_display", fixed_2(weight_g / weight_divisor) + weight_unit},
                                       {"length_mm", length_m * 1000.0},
                                       {"weight_g", weight_g}};
        for (size_t mode = 0; mode < travel_times.size(); ++mode) {
            const auto  time_it            = role_times[mode].find(role);
            const float seconds            = time_it == role_times[mode].end() ? 0.0f : time_it->second;
            const float total              = estimated.modes[mode].time;
            const float fraction           = total > 0.0f ? seconds / total : 0.0f;
            item["time"][mode_names[mode]] = {{"display", seconds > 0.0f ? time_display(seconds) : ""},
                                              {"percent_display", percent_display(fraction)},
                                              {"seconds", seconds},
                                              {"percent", fraction * 100.0f}};
        }
        record["roles"].push_back(std::move(item));
    }

    std::set<size_t> filament_ids;
    auto             collect_ids = [&filament_ids](const std::map<size_t, double>& volumes) {
        for (const auto& [id, volume] : volumes)
            filament_ids.insert(id);
    };
    collect_ids(estimated.model_volumes_per_extruder);
    collect_ids(estimated.support_volumes_per_extruder);
    collect_ids(estimated.flush_per_filament);
    collect_ids(estimated.wipe_tower_volumes_per_extruder);
    collect_ids(estimated.total_volumes_per_extruder);

    double excluded_length_mm = 0.0;
    double excluded_weight_g  = 0.0;
    for (size_t id : filament_ids) {
        const double diameter = id < result.filament_diameters.size() ? result.filament_diameters[id] : 0.0;
        const double density  = id < result.filament_densities.size() ? result.filament_densities[id] : 0.0;
        auto         model    = usage(volume_for(estimated.model_volumes_per_extruder, id), diameter, density, imperial_units);
        auto         support  = usage(volume_for(estimated.support_volumes_per_extruder, id), diameter, density, imperial_units);
        auto         flushed  = usage(volume_for(estimated.flush_per_filament, id), diameter, density, imperial_units);
        auto         tower    = usage(volume_for(estimated.wipe_tower_volumes_per_extruder, id), diameter, density, imperial_units);
        // The GUI's Total column adds the displayed categories, which can differ from the raw processor total.
        const double displayed_total_volume = volume_for(estimated.model_volumes_per_extruder, id) +
                                              volume_for(estimated.support_volumes_per_extruder, id) +
                                              volume_for(estimated.flush_per_filament, id) +
                                              volume_for(estimated.wipe_tower_volumes_per_extruder, id);
        auto         total                  = usage(displayed_total_volume, diameter, density, imperial_units);
        total["processor_volume_mm3"]       = volume_for(estimated.total_volumes_per_extruder, id);
        excluded_length_mm += support["length_mm"].get<double>() + flushed["length_mm"].get<double>() + tower["length_mm"].get<double>();
        excluded_weight_g += support["weight_g"].get<double>() + flushed["weight_g"].get<double>() + tower["weight_g"].get<double>();
        record["filaments"].push_back(
            {{"filament_id", id + 1}, {"model", model}, {"support", support}, {"flushed", flushed}, {"tower", tower}, {"total", total}});
    }
    const double model_length_mm = print_statistics.total_used_filament - excluded_length_mm;
    const double model_weight_g  = print_statistics.total_weight - excluded_weight_g;
    record["model_filament"]     = {{"length_display", fixed_2(model_length_mm / length_divisor) + length_unit},
                                    {"weight_display", fixed_2(model_weight_g / weight_divisor) + weight_unit},
                                    {"length_mm", model_length_mm},
                                    {"weight_g", model_weight_g}};
    return record;
}

std::string slice_statistics_sidecar_path(const std::string& gcode_path)
{
    if (gcode_path.empty())
        throw std::invalid_argument("G-code path is empty");
    return gcode_path + ".slice_statistics.json";
}

void write_slice_statistics_sidecar(const std::string& gcode_path, const nlohmann::ordered_json& statistics)
{
    namespace fs = boost::filesystem;
    const fs::path    sidecar(slice_statistics_sidecar_path(gcode_path));
    const fs::path    temporary  = sidecar.parent_path() / fs::unique_path(".slice_statistics.%%%%-%%%%.tmp");
    const std::string serialized = statistics.dump(2) + '\n';

    try {
        boost::nowide::ofstream stream(temporary.string(), std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("Could not open slice statistics temporary file: " + temporary.string());
        stream << serialized;
        stream.close();
        if (!stream)
            throw std::runtime_error("Could not write slice statistics temporary file: " + temporary.string());
#ifdef _WIN32
        if (const std::error_code error = rename_file(temporary.string(), sidecar.string()))
            throw std::runtime_error("Could not replace slice statistics file: " + sidecar.string() + ": " + error.message());
#else
        fs::rename(temporary, sidecar);
#endif
    } catch (...) {
        boost::system::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

} // namespace Slic3r
