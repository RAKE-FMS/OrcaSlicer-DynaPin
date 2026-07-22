#include <catch2/catch_test_macros.hpp>

#include "libslic3r/DynaPin.hpp"
#include "libslic3r/DynaPinPreview.hpp"

#include <cstdio>
#include <fstream>

using namespace Slic3r;

namespace {

std::string write_gcode(const std::string& body)
{
    char path[L_tmpnam];
    std::tmpnam(path);
    std::ofstream file(path);
    file << body;
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

GCodeProcessorResult make_result(const std::string& filename)
{
    GCodeProcessorResult result;
    result.filename = filename;
    result.moves.push_back({2, EMoveType::Travel, erNone, 0, 0, Vec3f(0.f, 0.f, 0.f)});
    result.moves.push_back({5, EMoveType::Travel, erNone, 0, 0, Vec3f(10.f, 0.f, 0.f)});
    result.moves.push_back({6, EMoveType::Travel, erNone, 0, 0, Vec3f(20.f, 0.f, 0.f)});
    return result;
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
    config.origin_y            = 10.;
    config.origin_z            = 20.;
    config.row_pitch_y         = 1.;
    config.col_pitch_z         = 2.;
    config.pull_gcode.x_hook   = 100.;
    config.pull_gcode.x_latch  = 110.;
    config.pull_gcode.x_front  = 120.;
    config.pull_gcode.y_offset = -3.5;

    const std::string gcode = DynaPin::pull_gcode_for_pin(config, {2, 5});

    CHECK(gcode.find("; BEGIN_DYNAPIN_PULL ROW=2 COL=5\n") != std::string::npos);
    CHECK(gcode.find("; DYNAPIN_PULL_MOVE\nG1 X120") != std::string::npos);
    CHECK(gcode.find("; END_DYNAPIN_PULL\n") != std::string::npos);
    CHECK(gcode.find("row=") == std::string::npos);
    CHECK(gcode.find("col=") == std::string::npos);
    CHECK(gcode.find("G1 Y4.5000") != std::string::npos);
    CHECK(gcode.find("G1 X100.0000 Z30.0000") != std::string::npos);
}

TEST_CASE("DynaPin physical and pull coordinates are independent", "[DynaPin]")
{
    DynaPin::Config config;
    config.origin_y            = 14.;
    config.origin_z            = 5.;
    config.physical_origin_y   = 18.;
    config.physical_origin_z   = 4.;
    config.row_pitch_y         = 12.4;
    config.col_pitch_z         = 7.4;
    config.pull_gcode.x_hook   = 160.;
    config.pull_gcode.x_latch  = 165.;
    config.pull_gcode.x_front  = 30.;
    config.pull_gcode.y_offset = -3.5;

    const DynaPin::Pin pin{1, 1};
    CHECK(DynaPin::pin_y(config, pin) == 30.4);
    CHECK(DynaPin::pin_z(config, pin) == 11.4);

    const std::string gcode = DynaPin::pull_gcode_for_pin(config, pin);
    CHECK(gcode.find("G1 Y22.9000") != std::string::npos);
    CHECK(gcode.find("G1 X160.0000 Z12.4000") != std::string::npos);
    CHECK(gcode.find("G1 X165.0000") != std::string::npos);
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
    GCodeProcessorResult result = make_result(path);
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
    GCodeProcessorResult result = make_result(path);
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
    GCodeProcessorResult result = make_result(path);
    result.lines_ends           = line_ends(body);

    DynaPinPreviewState state;
    state.load(result);
    CHECK(state.events().empty());

    std::remove(path.c_str());
}
