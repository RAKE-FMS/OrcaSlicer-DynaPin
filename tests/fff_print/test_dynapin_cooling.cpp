#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/DynaPin.hpp"
#include "libslic3r/DynaPinLanding.hpp"
#include "libslic3r/ShortestPath.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode.hpp"
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

TEST_CASE("DynaPin landing paths preserve traversal and extrusion volume", "[DynaPin][Cooling]")
{
    const Polygon region = rectangle(2, -1, 2, 2);
    ExtrusionPath original(erSupportMaterialInterface, 0.08, 0.4, 0.2);
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
        CHECK(path->role() == erSupportMaterialInterface);
        if (path->is_dynapin_landing)
            landing_length += unscale<double>(path->length());
        const ExtrusionPath copied(path->polyline, *path);
        CHECK(copied.is_dynapin_landing == path->is_dynapin_landing);
    }
    CHECK(previous == original.last_point());
    CHECK(landing_length == Catch::Approx(4.0));
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
