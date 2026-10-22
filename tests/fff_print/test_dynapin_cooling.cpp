#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/DynaPin.hpp"
#include "libslic3r/DynaPinLanding.hpp"
#include "libslic3r/ShortestPath.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/CoolingBuffer.hpp"
#include "libslic3r/GCode/FanMover.hpp"

#include <sstream>
#include <map>
#include <limits>

using namespace Slic3r;

namespace {
Polygon rectangle(double x, double y, double width, double height)
{
    return Polygon(Points{Point(scale_(x), scale_(y)), Point(scale_(x + width), scale_(y)), Point(scale_(x + width), scale_(y + height)),
                          Point(scale_(x), scale_(y + height))});
}

void check_fan(const std::string& gcode, int expected_outside, bool check_ordinary = true)
{
    INFO(gcode);
    bool               protected_region = false;
    bool               stop_command     = false;
    int                fan              = 0;
    size_t             protected_moves  = 0;
    size_t             ordinary_moves   = 0;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        if (line == DynaPin::LANDING_FAN_START) {
            protected_region = true;
            stop_command     = false;
        } else if (line == DynaPin::LANDING_FAN_END)
            protected_region = false;
        else if (line.rfind("M106 S", 0) == 0) {
            fan = std::stoi(line.substr(6));
            if (protected_region && fan == 0)
                stop_command = true;
        } else if (line.rfind("G1 X", 0) == 0) {
            if (protected_region) {
                CHECK(fan == 0);
                CHECK(stop_command);
                ++protected_moves;
            } else {
                if (check_ordinary)
                    CHECK(fan == expected_outside);
                ++ordinary_moves;
            }
        }
    }
    CHECK_FALSE(protected_region);
    CHECK(protected_moves == 2);
    CHECK(ordinary_moves >= 2);
}
} // namespace

TEST_CASE("DynaPin landing depth counts real Z layers per pin", "[DynaPin][Cooling]")
{
    const Polygon area = rectangle(0, 0, 10, 10);
    // The first slab crosses the pin top, and coincident slabs must not count twice.
    const std::vector<DynaPin::LandingLayer>          layers{{9.9, 10.1, {area}},
                                                             {10.1, 10.3, {area}},
                                                             {10.1, 10.3, {area}},
                                                             {10.3, 10.6, {area}},
                                                             {10.6, 11.0, {area}}};
    const std::vector<DynaPin::VirtualSupportSurface> surfaces{{{0, 0}, area, 10.0}, {{1, 0}, area, 10.4}};
    const size_t                                      count = GENERATE(0, 1, 2, 3, 6);
    const auto                                        masks = DynaPin::landing_regions(layers, surfaces, count);
    REQUIRE(masks.size() == layers.size());
    CHECK(masks[0].empty());
    CHECK(masks[1].empty() == (count == 0));
    CHECK(masks[2].empty() == (count == 0));
    CHECK(masks[3].empty() == (count < 2));
    CHECK(masks[4].empty() == (count == 0)); // This is also the upper pin's first layer.
    for (const Polygons& mask : masks)
        CHECK(diff(mask, {area}).empty());
}

TEST_CASE("DynaPin landing depth accepts touching slabs and stops at gaps", "[DynaPin][Cooling]")
{
    const Polygon                            area = rectangle(0, 0, 10, 10);
    const std::vector<DynaPin::LandingLayer> layers{{10., 10.2, {area}}, {11., 11.2, {area}}};
    const auto                               masks = DynaPin::landing_regions(layers, {{{0, 0}, area, 10.}}, 3);
    CHECK_FALSE(masks[0].empty());
    CHECK(masks[1].empty());
}

TEST_CASE("DynaPin ignores support beginning far above an unused pin", "[DynaPin][Cooling]")
{
    const Polygon area  = rectangle(0, 0, 10, 10);
    const auto    masks = DynaPin::landing_regions({{50., 50.2, {area}}}, {{{0, 0}, area, 10.}}, 3);
    CHECK(masks.front().empty());
}

TEST_CASE("DynaPin stabilization starts after each landing and stops after three real layers", "[DynaPin][SupportSpeed]")
{
    const Polygon                            left = rectangle(0, 0, 10, 10), right = rectangle(20, 0, 10, 10);
    const std::vector<DynaPin::LandingLayer> layers{{10., 10.2, {left}},         {10.2, 10.4, {left, right}}, {10.4, 10.6, {left, right}},
                                                    {10.6, 10.8, {left, right}}, {10.8, 11., {left, right}},  {11., 11.2, {left, right}}};
    const std::vector<Polygons>              bases{{left}, {right}, {}, {}, {}, {}};
    const auto                               masks = DynaPin::stabilization_regions(layers, bases, 3);
    REQUIRE(masks.size() == layers.size());
    CHECK(masks[0].empty());
    CHECK(intersection(masks[1], {left}).size() == 1);
    CHECK(intersection(masks[1], {right}).empty());
    CHECK(intersection(masks[3], {left}).size() == 1);
    CHECK(intersection(masks[4], {left}).empty());
    CHECK(intersection(masks[4], {right}).size() == 1);
    CHECK(masks[5].empty());
    const auto disabled = DynaPin::stabilization_regions(layers, bases, 0);
    CHECK(std::all_of(disabled.begin(), disabled.end(), [](const Polygons& mask) { return mask.empty(); }));
}

TEST_CASE("DynaPin stabilization stops at gaps and counts coincident Z only once", "[DynaPin][SupportSpeed]")
{
    const Polygon                            area = rectangle(0, 0, 10, 10);
    const std::vector<DynaPin::LandingLayer> layers{{10., 10.2, {area}},  {10.2, 10.4, {area}}, {10.2, 10.4, {area}},
                                                    {10.4, 10.6, {area}}, {10.6, 10.8, {area}}, {10.8, 11., {area}}};
    const std::vector<Polygons>              bases{{area}, {}, {}, {}, {}, {}};
    const auto                               masks = DynaPin::stabilization_regions(layers, bases, 3);
    CHECK(masks[0].empty());
    for (size_t i = 1; i <= 4; ++i)
        CHECK_FALSE(masks[i].empty());
    CHECK(masks[5].empty());
    const auto gap = DynaPin::stabilization_regions({{10., 10.2, {area}}, {10.4, 10.6, {area}}}, {{area}, {}}, 3);
    CHECK(gap[1].empty());
}

TEST_CASE("DynaPin stabilization never consumes support below the landing top", "[DynaPin][SupportSpeed]")
{
    const Polygon                            area = rectangle(0, 0, 10, 10);
    const std::vector<DynaPin::LandingLayer> layers{{10., 10.1, {area}},  {10., 10.2, {area}},  {10.2, 10.4, {area}},
                                                    {10.4, 10.6, {area}}, {10.6, 10.8, {area}}, {10.8, 11., {area}}};
    const auto                               masks = DynaPin::stabilization_regions(layers, {{}, {area}, {}, {}, {}, {}}, 3);
    CHECK(masks[0].empty());
    CHECK(masks[1].empty());
    CHECK_FALSE(masks[2].empty());
    CHECK_FALSE(masks[3].empty());
    CHECK_FALSE(masks[4].empty());
    CHECK(masks[5].empty());
}

TEST_CASE("DynaPin support speed caps X extrusion components regardless of length", "[DynaPin][SupportSpeed]")
{
    const Point origin(0, 0);
    CHECK(DynaPin::support_segment_speed(Line(origin, Point::new_scale(10., 0.)), 80., 20.) == Catch::Approx(20.));
    CHECK(DynaPin::support_segment_speed(Line(origin, Point::new_scale(-10., 0.)), 80., 20.) == Catch::Approx(20.));
    CHECK(DynaPin::support_segment_speed(Line(origin, Point::new_scale(0., 10.)), 80., 20.) == Catch::Approx(80.));
    CHECK(DynaPin::support_segment_speed(Line(origin, Point::new_scale(4.9, 0.)), 80., 20.) == Catch::Approx(20.));
    CHECK(DynaPin::support_segment_speed(Line(origin, Point::new_scale(5., 0.)), 80., 20.) == Catch::Approx(20.));
    CHECK(DynaPin::support_segment_speed(Line(origin, Point::new_scale(0.05, 0.)), 80., 20.) == Catch::Approx(20.));
    CHECK(DynaPin::support_segment_speed(Line(origin, Point::new_scale(0.3, 0.4)), 80., 20.) == Catch::Approx(100. / 3.));
    CHECK(DynaPin::support_segment_speed(Line(origin, Point::new_scale(6., 8.)), 80., 20.) == Catch::Approx(100. / 3.));
    CHECK(DynaPin::support_segment_speed(Line(origin, Point::new_scale(10., 0.)), 10., 20.) == Catch::Approx(10.));
}

TEST_CASE("DynaPin landing layers start on each pin without moving their upper connection", "[DynaPin][LandingZ]")
{
    const Polygon                            left  = rectangle(0, 0, 5, 5);
    const Polygon                            right = rectangle(10, 0, 5, 5);
    const double                             pin_z = GENERATE(10., 10.05, 10.149, 10.299);
    const std::vector<DynaPin::LandingLayer> layers{{10.3, 10.6, {left, right}}, {10.6, 10.8, {left, right}}};
    const auto changes = DynaPin::align_landing_layers(layers, {{{0, 0}, left, pin_z}, {{0, 1}, right, 10.3}}, 0.08, 0.3);
    REQUIRE(changes.size() == 1);
    CHECK(changes.front().source == 0);
    double bottom = pin_z;
    for (const auto& layer : changes.front().layers) {
        CHECK(layer.bottom_z == Catch::Approx(bottom));
        CHECK(layer.print_z - layer.bottom_z >= 0.08 - EPSILON);
        CHECK(layer.print_z - layer.bottom_z <= 0.3 + EPSILON);
        CHECK(diff(layer.polygons, {left}).empty());
        CHECK(intersection(layer.polygons, {right}).empty());
        bottom = layer.print_z;
    }
    CHECK(bottom == Catch::Approx(layers.front().print_z));
    CHECK(DynaPin::align_landing_layers(layers, {{{0, 0}, left, 1.}}, 0.08, 0.3).empty());
    CHECK(DynaPin::align_landing_layers(layers, {}, 0.08, 0.3).empty());
}

TEST_CASE("DynaPin base expansion stays on pins and separates different first heights", "[DynaPin][LandingBase]")
{
    const Polygon                            pin   = rectangle(0, 0, 8, 8);
    const Polygon                            first = rectangle(1, 1, 2, 2);
    const Polygon                            other = rectangle(5, 1, 2, 2);
    const std::vector<DynaPin::LandingLayer> layers{{10., 10.2, {first}}, {10., 10.2, {first}}, {10., 10.3, {other}}, {10.3, 10.5, {pin}}};
    const auto                               bases = DynaPin::landing_base_layers(layers, {{{0, 0}, pin, 10.}}, 2.);
    REQUIRE(bases.size() == 2);
    for (const auto& base : bases) {
        CHECK(base.bottom_z == 10.);
        CHECK(diff(base.polygons, {pin}).empty());
    }
    CHECK(intersection(bases[0].polygons, bases[1].polygons).empty());
    CHECK(diff({first}, bases[0].polygons).empty());
    CHECK(diff({other}, bases[1].polygons).empty());
    const auto unexpanded = DynaPin::landing_base_layers(layers, {{{0, 0}, pin, 10.}}, 0.);
    REQUIRE(unexpanded.size() == 2);
    CHECK(area(unexpanded[0].polygons) == Catch::Approx(first.area()));
    CHECK(DynaPin::landing_base_layers({{10.1, 10.3, {first}}}, {{{0, 0}, pin, 10.}}, 2.).empty());
}

TEST_CASE("DynaPin base fills a printable island between raster columns", "[DynaPin][LandingBase]")
{
    ExtrusionEntityCollection collection;
    const Flow                flow(0.6, 0.2, 0.4);
    const Polygon             island = rectangle(0.2, 0., 0.65, 1.5);
    DynaPin::fill_landing_paths(collection.entities, {island}, flow, 0.45, 0);
    REQUIRE_FALSE(collection.empty());
    for (const auto* entity : collection.entities) {
        CHECK(entity->role() == erSupportMaterial);
        CHECK(diff_pl(entity->as_polyline(), offset(Polygons{island}, -0.5 * flow.scaled_width() + SCALED_EPSILON)).empty());
    }
}

TEST_CASE("DynaPin base insets only exposed edges of contiguous pins", "[DynaPin][LandingBase][PinEdges]")
{
    const auto   columns = GENERATE(std::vector<int>{0}, std::vector<int>{0, 1}, std::vector<int>{0, 1, 2}, std::vector<int>{0, 2});
    const double shift   = GENERATE(0., -31.25);
    std::vector<DynaPin::VirtualSupportSurface> surfaces;
    std::vector<DynaPin::LandingLayer>          layers;
    Polygons                                    expected;
    for (const int column : columns) {
        const Polygon pin = rectangle(0., shift + 12.4 * column, 20., 12.4);
        surfaces.push_back({{0, column}, pin, 10., 1.2});
        layers.push_back({10., 10.2, {pin}});
        const bool left  = std::find(columns.begin(), columns.end(), column - 1) == columns.end();
        const bool right = std::find(columns.begin(), columns.end(), column + 1) == columns.end();
        expected.push_back(rectangle(0., shift + 12.4 * column + (left ? 1.2 : 0.), 20., 12.4 - (left ? 1.2 : 0.) - (right ? 1.2 : 0.)));
    }
    const auto bases = DynaPin::landing_base_layers(layers, surfaces, 100.);
    REQUIRE(bases.size() == 1);
    CHECK(diff(bases.front().polygons, expected).empty());
    CHECK(diff(expected, bases.front().polygons).empty());
    CHECK(union_ex(bases.front().polygons).size() == (columns.back() == 2 && columns.size() == 2 ? 2 : 1));
    const Flow                flow(0.6, 0.2, 0.4);
    ExtrusionEntityCollection paths;
    DynaPin::fill_landing_paths(paths.entities, bases.front().polygons, flow, 0.9, 0);
    REQUIRE_FALSE(paths.empty());
    const auto safe = offset(union_(expected), -0.5 * flow.scaled_width() + SCALED_EPSILON);
    for (const auto* path : paths.entities)
        CHECK(diff_pl(path->as_polyline(), safe).empty());
    for (auto& surface : surfaces)
        surface.edge_inset_y = 0.;
    const auto legacy = DynaPin::landing_base_layers(layers, surfaces, 100.);
    Polygons   nominal;
    for (const auto& surface : surfaces)
        nominal.push_back(surface.poly);
    CHECK(diff(nominal, legacy.front().polygons).empty());
}

TEST_CASE("DynaPin base expands across shared pin edges but not different heights", "[DynaPin][LandingBase][PinEdges]")
{
    std::vector<DynaPin::VirtualSupportSurface> surfaces{{{0, 0}, rectangle(0., 0., 20., 12.4), 10., 1.2},
                                                         {{0, 1}, rectangle(0., 12.4, 20., 12.4), 10., 1.2}};
    const std::vector<DynaPin::LandingLayer>    layers{{10., 10.2, {rectangle(5., 11.5, 1., 0.5)}}};
    const auto                                  joined = DynaPin::landing_base_layers(layers, surfaces, 2.);
    REQUIRE(joined.size() == 1);
    CHECK(unscale_(get_extents(joined.front().polygons).max.y()) > 12.4);
    surfaces.back().pin.row = 1;
    surfaces.back().print_z = 17.4;
    const auto separated    = DynaPin::landing_base_layers(layers, surfaces, 2.);
    REQUIRE(separated.size() == 1);
    CHECK(unscale_(get_extents(separated.front().polygons).max.y()) == Catch::Approx(11.2));
}

TEST_CASE("DynaPin landing paths preserve traversal and extrusion volume", "[DynaPin][Cooling]")
{
    const Polygon region = rectangle(2, -1, 2, 2);
    const auto    role   = GENERATE(erSupportMaterial, erSupportMaterialInterface);
    ExtrusionPath original(role, 0.08, 0.4, 0.2);
    original.polyline = Polyline(Points{Point(scale_(0), scale_(0)), Point(scale_(6), scale_(0)), Point(scale_(0), scale_(0))});
    ExtrusionEntityCollection collection;
    collection.append(original);
    DynaPin::mark_landing_paths(collection, {region});
    CHECK(collection.total_volume() == Catch::Approx(original.total_volume()));
    REQUIRE(collection.entities.size() == 1);
    const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(collection.entities.front());
    REQUIRE(multipath != nullptr);
    REQUIRE(multipath->paths.size() == 5);
    Point  previous       = original.first_point();
    double landing_length = 0;
    for (const ExtrusionPath& part : multipath->paths) {
        const auto* path = &part;
        CHECK(path->first_point() == previous);
        previous = path->last_point();
        CHECK(path->role() == role);
        if (path->is_dynapin_landing)
            landing_length += unscale<double>(path->length());
        const ExtrusionPath copied(path->polyline, *path);
        CHECK(copied.is_dynapin_landing == path->is_dynapin_landing);
    }
    CHECK(previous == original.last_point());
    CHECK(landing_length == Catch::Approx(4.0));
}

TEST_CASE("DynaPin mixed support speeds survive cooling and fan look-ahead", "[DynaPin][SupportSpeed][Cooling]")
{
    const bool  slowdown = GENERATE(false, true);
    GCode       generator;
    PrintConfig config;
    config.slow_down_for_layer_cooling.values = {slowdown};
    config.slow_down_layer_time.values        = {4.};
    config.fan_min_speed.values               = {40};
    config.fan_max_speed.values               = {40};
    generator.apply_print_config(config);
    generator.writer().set_extruders({0});
    CoolingBuffer     cooling(generator);
    const std::string source = "G1 F4200;_EXTRUDE_SET_SPEED\nG1 F1200;_EXTRUDE_SET_SPEED\nG1 X100 E1\n"
                               "G1 F4200;_EXTRUDE_SET_SPEED\nG1 Y10 E1\n"
                               "G1 F1200;_EXTRUDE_SET_SPEED\nG1 X200 E1\n;_EXTRUDE_END\n";
    const std::string cooled = cooling.process_layer(std::string(source), 10, true);
    FanMover          mover(generator.writer(), 1.f, true, false, false, 0.f);
    const std::string output = mover.process_gcode(cooled, true);
    INFO(output);
    GCodeReader reader;
    reader.parse_buffer(output, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
        if (line.cmd_is("G1") && line.has_y())
            CHECK(line.new_F(state) == Catch::Approx(4200.));
    });
}

TEST_CASE("DynaPin cooling overrides role fans and survives fan look-ahead", "[DynaPin][Cooling]")
{
    const int  interface_fan    = GENERATE(-1, 0, 100);
    const bool first_layers_off = GENERATE(false, true);
    const bool overhang_fan     = GENERATE(false, true);
    CAPTURE(interface_fan, first_layers_off, overhang_fan);
    GCode       generator;
    PrintConfig config;
    config.fan_min_speed.values                        = {40};
    config.fan_max_speed.values                        = {40};
    config.overhang_fan_speed.values                   = {80};
    config.reduce_fan_stop_start_freq.values           = {true};
    config.support_material_interface_fan_speed.values = {interface_fan};
    config.close_fan_the_first_x_layers.values         = {first_layers_off ? 5 : 0};
    config.slow_down_for_layer_cooling.values          = {false};
    generator.apply_print_config(config);
    generator.writer().set_extruders({0});
    CoolingBuffer     cooling(generator);
    const std::string ordinary = "G1 X1 E1 F600\n";
    const std::string landing  = std::string(DynaPin::LANDING_FAN_START) + "\nG1 X2 E2 F600\n;_FORCE_RESUME_FAN_SPEED\nG1 X3 E3 F600\n" +
                                 DynaPin::LANDING_FAN_END + "\n";
    const std::string source   = std::string(overhang_fan ? ";_OVERHANG_FAN_START\n" : "") + ";_SUPP_INTERFACE_FAN_START\n" + ordinary +
                                 landing + ordinary + ";_SUPP_INTERFACE_FAN_END\n";
    const size_t      split_at = source.find("G1 X3");
    CHECK(cooling.process_layer(source.substr(0, split_at), 2, false).empty());
    std::string processed = cooling.process_layer(source.substr(split_at), 2, true);
    const int   speed     = overhang_fan ? 80 : interface_fan < 0 ? 40 : interface_fan;
    const int   expected  = first_layers_off ? 0 : int(255.5 * speed / 100);
    check_fan(processed, expected);
    for (const float advance : {0.f, 0.5f, 5.f}) {
        FanMover     mover(generator.writer(), advance, true, false, false, 0.3f);
        const size_t boundary = processed.find("G1 X3");
        std::string  output   = mover.process_gcode(processed.substr(0, boundary), false);
        output += mover.process_gcode(processed.substr(boundary), true);
        check_fan(output, expected, false);
    }
}

TEST_CASE("DynaPin fan stays stopped between consecutive landing paths", "[DynaPin][Cooling][LandingFanContinuity]")
{
    const bool  ordinary_between = GENERATE(false, true);
    const bool  relative_e       = GENERATE(false, true);
    GCode       generator;
    PrintConfig config;
    config.fan_min_speed.values                = {40};
    config.fan_max_speed.values                = {40};
    config.reduce_fan_stop_start_freq.values   = {true};
    config.close_fan_the_first_x_layers.values = {0};
    config.slow_down_for_layer_cooling.values  = {false};
    config.use_relative_e_distances.value      = relative_e;
    config.auxiliary_fan.value = true;
    config.additional_cooling_fan_speed.values = {25};
    generator.apply_print_config(config);
    generator.writer().set_extruders({0});
    CoolingBuffer     cooling(generator);
    const std::string source    = std::string(DynaPin::LANDING_FAN_START) + "\nG1 X1 E1 F600\n;_EXTRUDE_END\n" + DynaPin::LANDING_FAN_END +
                                  (relative_e ? "\nG1 E-0.2\n;WIPE_START\nG1 X1.1 E-0.1\n;WIPE_END\nG1 X1.2\nG1 E0.3\n" :
                                                "\nG1 E0.8\n;WIPE_START\nG1 X1.1 E0.7\n;WIPE_END\nG1 X1.2\nG1 E1\n") +
                                  (ordinary_between ? "G1 X1.5 E1.5\n;_EXTRUDE_END\n" : "") + DynaPin::LANDING_FAN_START +
                                  "\nG1 X2 E2\n;_EXTRUDE_END\n" + DynaPin::LANDING_FAN_END + "\nG1 X3 E3\n;_EXTRUDE_END\n";
    const auto        processed = cooling.process_layer(std::string(source), 2, true);
    CHECK(processed.find("M106 P2 S63") != std::string::npos);
    CHECK(processed.find("M106 S102", processed.rfind(DynaPin::LANDING_FAN_END)) != std::string::npos);
    for (const float advance : {0.f, 0.5f, 5.f}) {
        FanMover   mover(generator.writer(), advance, true, false, false, 0.3f);
        const auto output = mover.process_gcode(processed, true);
        INFO(output);
        size_t             starts           = 0;
        bool               protected_region = false;
        int                fan              = -1;
        std::istringstream stream(output);
        for (std::string line; std::getline(stream, line);) {
            if (line == DynaPin::LANDING_FAN_START) {
                protected_region = true;
                ++starts;
            } else if (line == DynaPin::LANDING_FAN_END)
                protected_region = false;
            else if (line.rfind("M106 S", 0) == 0) {
                fan = std::stoi(line.substr(6));
                if (protected_region)
                    CHECK(fan == 0);
            } else if (line.rfind("G1 X1.2", 0) == 0 && !ordinary_between) {
                CHECK(protected_region);
                CHECK(fan == 0);
            } else if (line.rfind("G1 X3 E3", 0) == 0)
                CHECK(fan > 0); // Ordinary cooling may begin with a kickstart.
        }
        CHECK(starts == (ordinary_between ? 2 : 1));
        CHECK_FALSE(protected_region);
    }
}

TEST_CASE("DynaPin landing raster preserves columns, holes and flow fragments", "[DynaPin][LandingOrder]")
{
    const int  shape = GENERATE(0, 1, 2, 3);
    const Flow flow(0.44, 0.3, 0.4);
    Polygons   regions{rectangle(-4, -4, 8, 8)};
    if (shape == 1)
        regions = diff(regions, {rectangle(-1, -1, 2, 2)});
    else if (shape == 2)
        regions = diff(regions, {rectangle(-1, 0, 2, 5)});
    else if (shape == 3)
        regions = union_(regions, Polygons{rectangle(-2, -2, 6, 6)});
    ExtrusionEntityCollection collection;
    DynaPin::fill_landing_paths(collection.entities, regions, flow, 0.8, 0);
    REQUIRE_FALSE(collection.empty());
    const double raw_volume = collection.total_volume();
    // Simulate source-slab ordering and split-height fragments before final assembly.
    std::reverse(collection.entities.begin(), collection.entities.end());
    DynaPin::mark_landing_paths(collection, regions);
    DynaPin::order_landing_paths(collection, regions);
    REQUIRE(collection.entities.size() == 1);
    auto* ordered = dynamic_cast<ExtrusionEntityCollection*>(collection.entities.front());
    REQUIRE(ordered != nullptr);
    REQUIRE(DynaPin::ordered_landing_collection(*ordered));
    CHECK_FALSE(ordered->can_reverse());
    if (shape == 0 || shape == 3)
        CHECK(ordered->entities.size() == 1);
    CHECK(collection.total_volume() >= raw_volume);
    coord_t                 previous_x = std::numeric_limits<coord_t>::lowest();
    std::map<coord_t, bool> column_directions;
    for (const auto* entity : ordered->entities) {
        const auto* path = dynamic_cast<const ExtrusionPath*>(entity);
        REQUIRE(path != nullptr);
        CHECK(path->height == Catch::Approx(flow.height()));
        CHECK(path->mm3_per_mm == Catch::Approx(flow.mm3_per_mm()));
        CHECK(diff_pl(path->polyline, regions).empty());
        for (size_t i = 1; i < path->polyline.points.size(); ++i) {
            const Point a = path->polyline.points[i - 1], b = path->polyline.points[i];
            CHECK(a.x() >= previous_x);
            CHECK(b.x() >= a.x());
            previous_x = b.x();
            if (a.x() == b.x()) {
                const bool forward  = b.y() > a.y();
                auto       inserted = column_directions.emplace(a.x(), forward);
                CHECK(inserted.first->second == forward);
            }
        }
    }
    bool forward = true;
    for (const auto& column : column_directions) {
        CHECK(column.second == forward);
        forward = !forward;
    }
    const auto  original = ordered->as_polylines();
    auto        copy     = collection.flatten(true);
    const Point start(scale_(100), scale_(100));
    chain_and_reorder_extrusion_entities(copy.entities, &start);
    const auto* copied = dynamic_cast<const ExtrusionEntityCollection*>(copy.entities.front());
    REQUIRE(copied != nullptr);
    CHECK(copied->as_polylines() == original);
}

TEST_CASE("DynaPin landing order combines sources without changing ordinary paths", "[DynaPin][LandingOrder]")
{
    const Polygons            regions{rectangle(0, 0, 8, 8), rectangle(12, 0, 8, 8)};
    ExtrusionEntityCollection collection;
    const Flow                flow(0.44, 0.3, 0.4);
    DynaPin::fill_landing_paths(collection.entities, regions, flow, 1., 0);
    DynaPin::mark_landing_paths(collection, regions);
    // Split one column in traversal order, retaining different extrusion heights.
    auto* source = dynamic_cast<ExtrusionPath*>(collection.entities.front());
    REQUIRE(source != nullptr);
    const Point   mid(source->first_point().x(), (source->first_point().y() + source->polyline.points[1].y()) / 2);
    ExtrusionPath fragment(*source);
    fragment.polyline.points.front() = mid;
    fragment.height                  = 0.1f;
    fragment.mm3_per_mm /= 3.;
    source->polyline.points = Points{source->first_point(), mid};
    collection.append(fragment);
    ExtrusionPath ordinary(erSupportMaterial, 0.08, 0.4, 0.2);
    ordinary.polyline = Polyline(Points{Point::new_scale(-10, 0), Point::new_scale(-8, 0)});
    collection.append(ordinary);
    DynaPin::order_landing_paths(collection, regions);
    REQUIRE(collection.entities.size() == 3);
    CHECK(collection.entities.front()->as_polyline() == ordinary.polyline);
    bool reduced_flow_found = false;
    for (size_t i = 1; i < collection.entities.size(); ++i) {
        const auto* ordered = dynamic_cast<const ExtrusionEntityCollection*>(collection.entities[i]);
        REQUIRE(ordered != nullptr);
        CHECK(DynaPin::ordered_landing_collection(*ordered));
        coord_t previous = std::numeric_limits<coord_t>::lowest();
        for (const auto* entity : ordered->entities) {
            auto check = [&](const ExtrusionPath& path) {
                CHECK(path.first_point().x() >= previous);
                previous = path.last_point().x();
                reduced_flow_found |= path.height == fragment.height;
            };
            if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity))
                check(*path);
            else {
                const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(entity);
                REQUIRE(multi != nullptr);
                for (const auto& path : multi->paths)
                    check(path);
            }
        }
    }
    CHECK(reduced_flow_found);
}

TEST_CASE("DynaPin retains modulated flow along column connectors", "[DynaPin][LandingOrder]")
{
    ExtrusionEntityCollection collection;
    const Points              points{Point::new_scale(0, 0),   Point::new_scale(0, 4), Point::new_scale(0.4, 4),
                                     Point::new_scale(0.6, 4), Point::new_scale(1, 4), Point::new_scale(1, 0)};
    for (size_t i = 1; i < points.size(); ++i) {
        const bool    reduced = i == 3;
        ExtrusionPath path(erSupportMaterialInterface, reduced ? 0.02 : 0.08, 0.4, reduced ? 0.1 : 0.3);
        path.polyline           = Polyline(Points{points[i - 1], points[i]});
        path.is_dynapin_landing = true;
        collection.append(path);
    }
    const double volume = collection.total_volume();
    DynaPin::order_landing_paths(collection, {rectangle(-1, -1, 3, 6)});
    REQUIRE(collection.entities.size() == 1);
    const auto* ordered = dynamic_cast<const ExtrusionEntityCollection*>(collection.entities.front());
    REQUIRE(ordered != nullptr);
    REQUIRE(ordered->entities.size() == 1);
    const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(ordered->entities.front());
    REQUIRE(multi != nullptr);
    CHECK_FALSE(multi->can_reverse());
    CHECK(collection.total_volume() == Catch::Approx(volume));
    Point previous      = points.front();
    bool  reduced_found = false;
    for (const auto& path : multi->paths) {
        CHECK(path.first_point() == previous);
        previous = path.last_point();
        reduced_found |= path.mm3_per_mm == 0.02;
    }
    CHECK(previous == points.back());
    CHECK(reduced_found);
}

TEST_CASE("DynaPin shared landing raster respects spacing across model phases", "[DynaPin][LandingSpacing]")
{
    const Flow                             flow(0.5, 0.2, 0.4);
    const double                           density = GENERATE(0.8, 1.0);
    std::vector<DynaPin::LandingExtrusion> input;
    std::vector<DynaPin::LandingRegion>    regions;
    for (size_t source = 0; source < 2; ++source) {
        ExtrusionEntityCollection collection;
        regions.push_back({source, flow.height(), {rectangle(source * 3., 0., 6., 4.)}});
        DynaPin::fill_landing_paths(collection.entities, {rectangle(source * 3., 0., 6., 4.)}, flow, density, scale_(source * 0.073));
        ExtrusionPaths paths;
        DynaPin::collect_landing_paths(collection, paths);
        for (auto& path : paths) {
            input.push_back({source, std::move(path)});
        }
    }
    const Polygons safe_centers = offset(Polygons{rectangle(0., 0., 9., 4.)}, -0.5 * flow.scaled_width() + SCALED_EPSILON);
    const auto     output       = DynaPin::schedule_landing_paths(std::move(input), false, {density, density}, regions);
    REQUIRE_FALSE(output.empty());
    coord_t left = std::numeric_limits<coord_t>::max(), right = std::numeric_limits<coord_t>::lowest();
    for (size_t i = 0; i < output.size(); ++i) {
        const auto& a = output[i].path;
        left          = std::min(left, a.first_point().x());
        right         = std::max(right, a.first_point().x());
        CHECK(a.is_dynapin_landing);
        CHECK(a.height == flow.height());
        CHECK(a.mm3_per_mm == flow.mm3_per_mm());
        CHECK(diff_pl(a.polyline, safe_centers).empty());
        for (size_t j = i + 1; j < output.size(); ++j) {
            const auto&   b         = output[j].path;
            const coord_t y_overlap = std::min(std::max(a.first_point().y(), a.last_point().y()),
                                               std::max(b.first_point().y(), b.last_point().y())) -
                                      std::max(std::min(a.first_point().y(), a.last_point().y()),
                                               std::min(b.first_point().y(), b.last_point().y()));
            if (y_overlap > SCALED_EPSILON)
                CHECK(unscale<double>(std::abs(a.first_point().x() - b.first_point().x())) >= flow.spacing() / density - 0.001);
        }
    }
    CHECK(unscale<double>(left) == Catch::Approx(0.25).margin(0.001));
    CHECK(unscale<double>(right) == Catch::Approx(8.75).margin(0.001));
}

TEST_CASE("DynaPin shared landing raster meets neighboring slope bands", "[DynaPin][LandingSpacing][LandingBandGap]")
{
    std::vector<DynaPin::LandingExtrusion> input;
    std::vector<DynaPin::LandingRegion>    regions;
    for (size_t source = 0; source < 2; ++source)
        for (size_t band = 0; band < 2; ++band) {
            const Flow     flow(0.5, band == 0 ? 0.1 : 0.2, 0.4);
            const double   left  = std::max(source * 5., band * 7.5);
            const double   right = std::min(source * 5. + 10., band * 7.5 + 7.5);
            const Polygons area{rectangle(left, 0., right - left, 4.)};
            regions.push_back({source, flow.height(), area});
            ExtrusionEntityCollection collection;
            DynaPin::fill_landing_paths(collection.entities, area, flow, 1., scale_(source * 0.073));
            ExtrusionPaths paths;
            DynaPin::collect_landing_paths(collection, paths);
            for (auto& path : paths)
                input.push_back({source, std::move(path)});
        }
    const auto output        = DynaPin::schedule_landing_paths(std::move(input), false, {1., 1.}, regions);
    double     left_band_end = 0., right_band_start = 15.;
    for (const auto& item : output) {
        const double x = unscale<double>(item.path.first_point().x());
        if (item.path.height < 0.15)
            left_band_end = std::max(left_band_end, x + item.path.width / 2.);
        else
            right_band_start = std::min(right_band_start, x - item.path.width / 2.);
    }
    CHECK(left_band_end == Catch::Approx(7.5).margin(0.001));
    CHECK(right_band_start == Catch::Approx(7.5).margin(0.001));
}

TEST_CASE("DynaPin shared landing keeps modulated flows within a slab", "[DynaPin][LandingSpacing]")
{
    std::vector<DynaPin::LandingExtrusion> input;
    for (size_t i = 0; i < 2; ++i) {
        ExtrusionPath path(erSupportMaterial, i == 0 ? 0.05 : 0.08, 0.5, 0.2);
        path.is_dynapin_landing = true;
        path.polyline           = Polyline(Points{Point::new_scale(1., i * 2.), Point::new_scale(1., i * 2. + 2.)});
        input.push_back({0, std::move(path)});
    }
    const auto output = DynaPin::schedule_landing_paths(std::move(input), false, {1.}, {{0, 0.2f, {rectangle(0., 0., 4., 4.)}}});
    REQUIRE(output.size() == 2);
    CHECK(output[0].path.mm3_per_mm == 0.05);
    CHECK(output[1].path.mm3_per_mm == 0.08);
    CHECK(output[0].path.total_volume() + output[1].path.total_volume() == Catch::Approx(0.26));
}

TEST_CASE("DynaPin layer landing schedule interleaves models in machine X order", "[DynaPin][LandingOrder]")
{
    std::vector<DynaPin::LandingExtrusion> input;
    double                                 volume = 0.;
    for (size_t source = 0; source < 2; ++source) {
        ExtrusionEntityCollection collection;
        const Flow                flow(source == 0 ? 0.44 : 0.6, source == 0 ? 0.2 : 0.3, 0.4);
        DynaPin::fill_landing_paths(collection.entities, {rectangle(0, 0, 4, 4)}, flow, 0.8, 0);
        ExtrusionPaths paths;
        DynaPin::collect_landing_paths(collection, paths);
        for (auto& path : paths) {
            path.polyline.translate(Point::new_scale(source == 0 ? 1. : 0., source == 0 ? 10. : 0.));
            volume += path.total_volume();
            input.push_back({source, std::move(path)});
        }
    }
    // An ordinary path must never enter the landing pass.
    ExtrusionEntityCollection ordinary;
    ExtrusionPath             ordinary_path(erSupportMaterial, 0.1, 0.5, 0.2);
    ordinary_path.polyline = Polyline(Points{Point::new_scale(0., 0.), Point::new_scale(4., 0.)});
    ordinary.append(ordinary_path);
    ExtrusionPaths collected;
    DynaPin::collect_landing_paths(ordinary, collected);
    CHECK(collected.empty());
    const auto ordered = DynaPin::schedule_landing_paths(std::move(input));
    REQUIRE_FALSE(ordered.empty());
    coord_t previous_x = std::numeric_limits<coord_t>::lowest();
    size_t  changes = 0, previous_source = ordered.front().source;
    double  scheduled_volume = 0.;
    for (const auto& item : ordered) {
        CHECK(item.path.first_point().x() >= previous_x);
        CHECK(item.path.last_point().x() >= item.path.first_point().x());
        previous_x = item.path.first_point().x();
        CHECK(item.path.is_dynapin_landing);
        CHECK(item.path.height == Catch::Approx(item.source == 0 ? 0.2 : 0.3));
        scheduled_volume += item.path.total_volume();
        changes += previous_source != item.source;
        previous_source = item.source;
    }
    CHECK(changes > 2);
    CHECK(scheduled_volume == Catch::Approx(volume));
}
