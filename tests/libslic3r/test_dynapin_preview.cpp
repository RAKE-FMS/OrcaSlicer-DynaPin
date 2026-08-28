#include <catch2/catch_all.hpp>

#include "libslic3r/DynaPin.hpp"
#include "libslic3r/DynaPinPreview.hpp"

#include <cstdio>
#include <fstream>
#include <unistd.h>

using namespace Slic3r;

namespace {

std::string write_gcode(const std::string& body)
{
    char path[] = "/tmp/orcaslicer-dynapin-XXXXXX";
    const int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);
    ::close(fd);
    std::ofstream file(path);
    file << body;
    file.close();
    return path;
}

std::vector<size_t> line_ends(const std::string& body)
{
    std::vector<size_t> ends;
    for (size_t i = 0; i < body.size(); ++i)
        if (body[i] == '\n')
            ends.push_back(i + 1);
    if (ends.empty() || ends.back() != body.size())
        ends.push_back(body.size());
    return ends;
}

void initialize_result(GCodeProcessorResult& result, const std::string& filename)
{
    result.filename = filename;
    result.moves.push_back({2, EMoveType::Travel, erNone, 0, 0, Vec3f(0.f, 0.f, 0.f)});
    result.moves.push_back({5, EMoveType::Travel, erNone, 0, 0, Vec3f(10.f, 0.f, 0.f)});
    result.moves.push_back({6, EMoveType::Travel, erNone, 0, 0, Vec3f(20.f, 0.f, 0.f)});
}

} // namespace

TEST_CASE("DynaPin model names are parsed", "[DynaPinPreview]")
{
    auto address = DynaPinPreviewState::parse_model_name("dynapin_r2_c5");
    REQUIRE(address);
    CHECK(address->row == 2);
    CHECK(address->col == 5);

    CHECK(!DynaPinPreviewState::parse_model_name("pin_r2_c5"));
    CHECK(!DynaPinPreviewState::parse_model_name("dynapin_r2"));
}

TEST_CASE("DynaPin pull G-code comments match preview contract", "[DynaPin]")
{
    DynaPin::Config config;
    config.pull_origin_y       = 10.;
    config.pull_origin_z       = 20.;
    config.row_pitch_z         = 2.;
    config.col_pitch_y         = 1.;
    config.pull_gcode.x_hook   = 100.;
    config.pull_gcode.x_latch  = 110.;
    config.pull_gcode.x_front  = 20.;
    config.pull_gcode.y_offset = -3.5;

    const std::string gcode = DynaPin::pull_gcode_for_pin(config, {5, 2});

    CHECK(gcode.find("; BEGIN_DYNAPIN_PULL ROW=5 COL=2\n") != std::string::npos);
    CHECK(gcode.find("; DYNAPIN_PULL_MOVE\nG1 X20") != std::string::npos);
    CHECK(gcode.find("; END_DYNAPIN_PULL\n") != std::string::npos);
    CHECK(gcode.find("row=") == std::string::npos);
    CHECK(gcode.find("col=") == std::string::npos);
    CHECK(gcode.find("G1 Y4.5000") != std::string::npos);
    CHECK(gcode.find("G1 X100.0000 Z30.0000") != std::string::npos);
}

TEST_CASE("DynaPin support and pull coordinates are independent", "[DynaPin]")
{
    DynaPin::Config config;
    config.pull_origin_y       = 14.;
    config.pull_origin_z       = 5.;
    config.support_origin_y    = 18.;
    config.support_origin_z    = 7.55;
    config.row_pitch_z         = 7.4;
    config.col_pitch_y         = 12.4;
    config.blocker_height_z    = 5.;
    config.pull_gcode.x_hook   = 160.;
    config.pull_gcode.x_latch  = 165.;
    config.pull_gcode.x_front  = 30.;
    config.pull_gcode.y_offset = -3.5;

    const DynaPin::Pin pin{1, 1};
    CHECK(DynaPin::pin_y(config, pin) == 30.4);
    const DynaPin::BlockerZRange range = DynaPin::blocker_z_range(config, pin);
    CHECK(range.z_max == Catch::Approx(14.95));
    CHECK(range.z_min == Catch::Approx(9.95));

    const std::string gcode = DynaPin::pull_gcode_for_pin(config, pin);
    CHECK(gcode.find("G1 Y22.9000") != std::string::npos);
    CHECK(gcode.find("G1 X160.0000 Z12.4000") != std::string::npos);
    CHECK(gcode.find("G1 X165.0000") != std::string::npos);

    CHECK(DynaPin::pin_y(config, {2, 0}) == 18.);
    CHECK(DynaPin::pin_y(config, {0, 2}) == 42.8);
    CHECK(DynaPin::blocker_z_range(config, {2, 0}).z_max == Catch::Approx(22.35));
}

TEST_CASE("DynaPin candidate grid starts at zero", "[DynaPin]")
{
    DynaPin::Config config;
    config.row_count = 14;
    config.col_count = 10;

    const std::vector<DynaPin::Pin> pins = DynaPin::candidate_pins(config);
    REQUIRE(pins.size() == 140);
    CHECK(pins.front() == DynaPin::Pin{0, 0});
    CHECK(pins.back() == DynaPin::Pin{13, 9});

    config.row_count = 0;
    CHECK(DynaPin::candidate_pins(config).empty());
    config.row_count = 14;
    config.col_count = -1;
    CHECK(DynaPin::candidate_pins(config).empty());
}

TEST_CASE("DynaPin pins are sorted by physical height and deduplicated", "[DynaPin]")
{
    DynaPin::Config config;
    config.support_origin_z = 5.;
    config.row_pitch_z      = 7.4;
    std::vector<DynaPin::Pin> pins{{2, 3}, {1, 3}, {2, 3}, {0, 5}};

    DynaPin::sort_unique_pins(pins, config);
    REQUIRE(pins.size() == 3);
    CHECK(pins[0] == DynaPin::Pin{0, 5});
    CHECK(pins[1] == DynaPin::Pin{1, 3});
    CHECK(pins[2] == DynaPin::Pin{2, 3});
}

TEST_CASE("DynaPin downward projection stops at the highest safe pin", "[DynaPin]")
{
    const Polygon landing{{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}};
    std::vector<DynaPin::ProjectionEvent> events{
        {DynaPin::ProjectionEventType::Contact, 30., {landing}, {}, false},
        {DynaPin::ProjectionEventType::PinSurface, 20., {landing}, {0, 2}, false},
        {DynaPin::ProjectionEventType::PinSurface, 10., {landing}, {0, 1}, false},
    };

    const DynaPin::ProjectionSelection result = DynaPin::select_from_projection(std::move(events));
    REQUIRE(result.selected.size() == 1);
    CHECK(result.selected.front() == DynaPin::Pin{0, 2});
    CHECK(result.rejected_collisions.empty());
}

TEST_CASE("DynaPin downward projection continues past a colliding pin", "[DynaPin]")
{
    const Polygon landing{{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}};
    std::vector<DynaPin::ProjectionEvent> events{
        {DynaPin::ProjectionEventType::Contact, 30., {landing}, {}, false},
        {DynaPin::ProjectionEventType::PinSurface, 20., {landing}, {0, 2}, true},
        {DynaPin::ProjectionEventType::PinSurface, 10., {landing}, {0, 1}, false},
    };

    const DynaPin::ProjectionSelection result = DynaPin::select_from_projection(std::move(events));
    REQUIRE(result.rejected_collisions.size() == 1);
    CHECK(result.rejected_collisions.front() == DynaPin::Pin{0, 2});
    REQUIRE(result.selected.size() == 1);
    CHECK(result.selected.front() == DynaPin::Pin{0, 1});
}

TEST_CASE("DynaPin selection ignores pin surfaces outside the support projection", "[DynaPin]")
{
    const Polygon support{{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}};
    const Polygon remote{{2000, 0}, {3000, 0}, {3000, 1000}, {2000, 1000}};
    const DynaPin::ProjectionSelection result = DynaPin::select_from_projection({
        {DynaPin::ProjectionEventType::Contact, 30., {support}, {}, false},
        {DynaPin::ProjectionEventType::PinSurface, 20., {remote}, {0, 2}, false},
    });

    CHECK(result.selected.empty());
    CHECK(result.rejected_collisions.empty());
}

TEST_CASE("DynaPin pull comments create events", "[DynaPinPreview]")
{
    const std::string    body   = "G1 X0\n"
                                  "; BEGIN_DYNAPIN_PULL ROW=2 COL=5\n"
                                  "G1 X5\n"
                                  "; DYNAPIN_PULL_MOVE\n"
                                  "G1 X10\n"
                                  "; END_DYNAPIN_PULL\n";
    const std::string    path   = write_gcode(body);
    GCodeProcessorResult result;
    initialize_result(result, path);
    result.lines_ends           = line_ends(body);

    DynaPinPreviewState state;
    state.load(result);

    REQUIRE(state.events().size() == 1);
    CHECK(state.events().front().address.row == 2);
    CHECK(state.events().front().address.col == 5);
    CHECK(state.events().front().pull_gcode_id == 5);
    CHECK(state.events().front().start_pos == Vec3f(0.f, 0.f, 0.f));
    CHECK(state.events().front().end_pos == Vec3f(10.f, 0.f, 0.f));

    std::remove(path.c_str());
}

TEST_CASE("DynaPin preview state applies only matching selections", "[DynaPinPreview]")
{
    const std::string    body   = "G1 X0\n"
                                  "; BEGIN_DYNAPIN_PULL ROW=2 COL=5\n"
                                  "G1 X5\n"
                                  "; DYNAPIN_PULL_MOVE\n"
                                  "G1 X10\n"
                                  "; END_DYNAPIN_PULL\n";
    const std::string    path   = write_gcode(body);
    GCodeProcessorResult result;
    initialize_result(result, path);
    result.lines_ends           = line_ends(body);

    DynaPinPreviewState state;
    state.load(result);
    state.set_selection({{2, 5}, Vec3f(1.f, 1.f, 1.f)});
    CHECK(state.position_for_gcode_id(3) == Vec3f(1.f, 1.f, 1.f));
    CHECK(state.position_for_gcode_id(5) == Vec3f(10.f, 0.f, 0.f));
    CHECK(state.position_for_gcode_id(6) == Vec3f(10.f, 0.f, 0.f));

    state.set_selection({{0, 0}, Vec3f(1.f, 1.f, 1.f)});
    CHECK(state.position_for_gcode_id(6) == Vec3f(1.f, 1.f, 1.f));

    std::remove(path.c_str());
}

TEST_CASE("DynaPin incomplete blocks are ignored", "[DynaPinPreview]")
{
    const std::string    body   = "; BEGIN_DYNAPIN_PULL ROW=2 COL=5\n"
                                  "G1 X5\n"
                                  "; END_DYNAPIN_PULL\n"
                                  "; BEGIN_DYNAPIN_PULL ROW=3 COL=1\n"
                                  "G1 X10\n"
                                  "; DYNAPIN_PULL_MOVE\n";
    const std::string    path   = write_gcode(body);
    GCodeProcessorResult result;
    initialize_result(result, path);
    result.lines_ends           = line_ends(body);

    DynaPinPreviewState state;
    state.load(result);
    CHECK(state.events().empty());

    std::remove(path.c_str());
}
