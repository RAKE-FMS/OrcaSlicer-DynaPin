#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SliceStatisticsLog.hpp"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <fstream>
#include <iterator>

using namespace Slic3r;

namespace {

void initialize_sample_result(GCodeProcessorResult& result)
{
    result.id                                      = 42;
    result.filament_diameters                      = {2.0f};
    result.filament_densities                      = {1.0f};
    result.print_statistics.modes[0].time          = 3661.0f;
    result.print_statistics.modes[0].prepare_time  = 61.0f;
    result.print_statistics.total_filament_changes = 2;
    result.print_statistics.total_extruder_changes = 1;

    const double area                                                           = PI;
    result.print_statistics.model_volumes_per_extruder[0]                       = 1000.0 * area;
    result.print_statistics.support_volumes_per_extruder[0]                     = 100.0 * area;
    result.print_statistics.flush_per_filament[0]                               = 50.0 * area;
    result.print_statistics.wipe_tower_volumes_per_extruder[0]                  = 25.0 * area;
    result.print_statistics.total_volumes_per_extruder[0]                       = 1175.0 * area;
    result.print_statistics.used_filaments_per_role[erPerimeter]                = {1.0, area};
    result.print_statistics.used_filaments_per_role[erSupportMaterial]          = {0.06, 60.0 * area * 0.001};
    result.print_statistics.used_filaments_per_role[erSupportMaterialInterface] = {0.04, 40.0 * area * 0.001};

    result.moves.emplace_back();
    result.moves.emplace_back();
    result.moves.back().type           = EMoveType::Extrude;
    result.moves.back().extrusion_role = erPerimeter;
    result.moves.back().time[0]        = 10.0f;
    result.moves.emplace_back();
    result.moves.back().type    = EMoveType::Travel;
    result.moves.back().time[0] = 5.0f;
    result.moves.emplace_back();
    result.moves.back().type           = EMoveType::Extrude;
    result.moves.back().extrusion_role = erSupportMaterial;
    result.moves.back().time[0]        = 3.0f;
    result.moves.emplace_back();
    result.moves.back().type           = EMoveType::Extrude;
    result.moves.back().extrusion_role = erSupportMaterialInterface;
    result.moves.back().time[0]        = 2.0f;
}

} // namespace

TEST_CASE("Slice statistics log accepts each successful plate result only once", "[SliceStatisticsLog]")
{
    GCodeProcessorResult first;
    initialize_sample_result(first);
    GCodeProcessorResult second;
    initialize_sample_result(second);
    second.id = 43;

    unsigned int last_logged_result_id = 0;
    CHECK_FALSE(should_log_slice_statistics(false, true, &first, last_logged_result_id));
    CHECK_FALSE(should_log_slice_statistics(true, false, &first, last_logged_result_id));
    CHECK_FALSE(should_log_slice_statistics(true, true, nullptr, last_logged_result_id));
    CHECK(should_log_slice_statistics(true, true, &first, last_logged_result_id));
    last_logged_result_id = first.id;
    CHECK_FALSE(should_log_slice_statistics(true, true, &first, last_logged_result_id));
    CHECK(should_log_slice_statistics(true, true, &second, last_logged_result_id));
}

TEST_CASE("Slice statistics log matches metric GUI formatting", "[SliceStatisticsLog]")
{
    GCodeProcessorResult result;
    initialize_sample_result(result);
    PrintStatistics print_statistics;
    print_statistics.total_used_filament   = 1175.0;
    print_statistics.total_weight          = 1175.0 * PI * 0.001;
    print_statistics.total_extruded_volume = 1175.0 * PI;
    print_statistics.total_cost            = 2.5;
    print_statistics.filament_stats[0]     = 1000.0 * PI;

    auto log = make_slice_statistics_log(result, print_statistics, 2, false);
    CHECK(log["plate_index"] == 2);
    CHECK(log["result_id"] == 42);
    CHECK(log["time"]["normal"]["total_display"] == "1h1m");
    CHECK(log["time"]["normal"]["prepare_display"] == "1m1s");
    CHECK(log["time"]["normal"]["model_display"] == "1h0m");
    CHECK(log["time"]["normal"]["travel"]["seconds"] == 5.0f);
    CHECK(log["time"]["normal"]["travel"]["percent_display"] == "0.1");
    CHECK(log["total_filament"]["length_display"] == "1.18m");
    CHECK(log["model_filament"]["length_display"] == "1.00m");
    CHECK(log["cost"]["display"] == "2.50");
    CHECK(log["filament_changes"] == 2);
    CHECK(log["details"]["filament_stats_volume_mm3"][0]["volume_mm3"].get<double>() == Catch::Approx(1000.0 * PI));
    REQUIRE(log["filaments"].size() == 1);
    CHECK(log["filaments"][0]["support"]["length_mm"].get<double>() == Catch::Approx(100.0));
    CHECK(log["filaments"][0]["flushed"]["length_display"] == "0.05m");

    const auto perimeter = std::find_if(log["roles"].begin(), log["roles"].end(),
                                        [](const auto& item) { return item["role"] == ExtrusionEntity::role_to_string(erPerimeter); });
    REQUIRE(perimeter != log["roles"].end());
    CHECK((*perimeter)["time"]["normal"]["seconds"] == 10.0f);
    CHECK((*perimeter)["length_display"] == "1.00m");

    const auto support = std::find_if(log["roles"].begin(), log["roles"].end(),
                                      [](const auto& item) { return item["role"] == ExtrusionEntity::role_to_string(erSupportMaterial); });
    const auto interface = std::find_if(log["roles"].begin(), log["roles"].end(), [](const auto& item) {
        return item["role"] == ExtrusionEntity::role_to_string(erSupportMaterialInterface);
    });
    REQUIRE(support != log["roles"].end());
    REQUIRE(interface != log["roles"].end());
    CHECK((*support)["length_mm"] == 60.0);
    CHECK((*support)["time"]["normal"]["seconds"] == 3.0f);
    CHECK((*interface)["length_mm"] == 40.0);
    CHECK((*interface)["time"]["normal"]["seconds"] == 2.0f);
    CHECK(log["filaments"][0]["support"]["length_mm"].get<double>() == Catch::Approx(100.0));
}

TEST_CASE("Slice statistics sidecar uses final G-code filename and replaces complete JSON", "[SliceStatisticsLog]")
{
    namespace fs       = boost::filesystem;
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("slice-statistics-%%%%-%%%%");
    fs::create_directory(dir);
    const std::string gcode   = (dir / "plate_1.gcode").string();
    const std::string sidecar = slice_statistics_sidecar_path(gcode);
    REQUIRE(sidecar == (dir / "plate_1.gcode.slice_statistics.json").string());

    GCodeProcessorResult result;
    initialize_sample_result(result);
    PrintStatistics print_statistics;
    const auto      statistics = make_slice_statistics_log(result, print_statistics, 1, false);
    write_slice_statistics_sidecar(gcode, statistics);
    std::ifstream first(sidecar);
    REQUIRE(first.good());
    CHECK(nlohmann::ordered_json::parse(first) == statistics);
    first.close();

    write_slice_statistics_sidecar(gcode, {{"plate_index", 2}});
    std::ifstream second(sidecar);
    REQUIRE(second.good());
    const auto saved = nlohmann::json::parse(second);
    CHECK(saved["plate_index"] == 2);
    CHECK_FALSE(saved.contains("roles"));
    second.close();

    const std::string percent_gcode = (dir / "plate_2%20.gcode").string();
    write_slice_statistics_sidecar(percent_gcode, {{"plate_index", 3}});
    CHECK(fs::exists(slice_statistics_sidecar_path(percent_gcode)));
    const std::string second_plate_gcode = (dir / "plate_2.gcode").string();
    write_slice_statistics_sidecar(second_plate_gcode, {{"plate_index", 2}});
    std::ifstream second_plate(slice_statistics_sidecar_path(second_plate_gcode));
    REQUIRE(second_plate.good());
    CHECK(nlohmann::json::parse(second_plate)["plate_index"] == 2);
    fs::remove_all(dir);
}

TEST_CASE("Slice statistics sidecar cleans temporary output on failure", "[SliceStatisticsLog]")
{
    namespace fs       = boost::filesystem;
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("slice-statistics-%%%%-%%%%");
    fs::create_directory(dir);
    const std::string gcode = (dir / "plate_1.gcode").string();
    const fs::path    sidecar(slice_statistics_sidecar_path(gcode));
    fs::create_directory(sidecar);
    std::ofstream marker((sidecar / "keep").string());
    marker << "existing";
    marker.close();

    CHECK_THROWS(write_slice_statistics_sidecar(gcode, {{"plate_index", 1}}));
    CHECK(fs::exists(sidecar / "keep"));
    CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator()) == 1);
    fs::remove_all(dir);
}

TEST_CASE("Slice statistics log includes imperial display and guards missing filament dimensions", "[SliceStatisticsLog]")
{
    GCodeProcessorResult result;
    initialize_sample_result(result);
    PrintStatistics print_statistics;
    print_statistics.total_used_filament = 1175.0;
    print_statistics.total_weight        = 1175.0 * PI * 0.001;
    auto log                             = make_slice_statistics_log(result, print_statistics, 1, true);
    CHECK(log["unit_system"] == "imperial");
    CHECK(log["total_filament"]["length_display"] == "46.26in");
    CHECK(log["filaments"][0]["support"]["length_display"] == "3.94in");
    CHECK(log["filaments"][0]["support"]["weight_display"] == "0.01oz");

    result.filament_diameters.clear();
    result.filament_densities.clear();
    log = make_slice_statistics_log(result, print_statistics, 1, false);
    CHECK(log["filaments"][0]["support"]["volume_mm3"].get<double>() > 0.0);
    CHECK(log["filaments"][0]["support"]["length_mm"] == 0.0);
}

TEST_CASE("Slice statistics JSON follows GUI section and column order", "[SliceStatisticsLog]")
{
    GCodeProcessorResult result;
    initialize_sample_result(result);
    PrintStatistics print_statistics;
    auto statistics = make_slice_statistics_log(result, print_statistics, 1, false);
    const auto serialized = statistics.dump(2);

    const auto roles = serialized.find("\"roles\"");
    const auto filaments = serialized.find("\"filaments\"");
    const auto total_filament = serialized.find("\"total_filament\"");
    const auto model_filament = serialized.find("\"model_filament\"");
    const auto cost = serialized.find("\"cost\"");
    const auto time = serialized.find("\n  \"time\":");
    const auto details = serialized.find("\"details\"");
    REQUIRE(roles < filaments);
    REQUIRE(filaments < total_filament);
    REQUIRE(total_filament < model_filament);
    REQUIRE(model_filament < cost);
    REQUIRE(cost < time);
    REQUIRE(time < details);
    CHECK_FALSE(statistics.contains("schema_version"));
    CHECK_FALSE(statistics.contains("initial_tool"));
    CHECK(statistics["details"].contains("initial_tool"));
    CHECK(statistics["details"].contains("filament_stats_volume_mm3"));
    CHECK(serialized.find("\"model\"") < serialized.find("\"support\""));
    CHECK(serialized.find("\"support\"") < serialized.find("\"flushed\""));
    CHECK(serialized.find("\"flushed\"") < serialized.find("\"tower\""));
}
