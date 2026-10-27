#include <catch2/catch_all.hpp>

#include <memory>

#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/GCodeReader.hpp"

using namespace Slic3r;

SCENARIO("lift() is not ignored after unlift() at normal values of Z", "[GCodeWriter][.]") {
    GIVEN("A config from a file and a single extruder.") {
        GCodeWriter writer;
        GCodeConfig &config = writer.config;
        config.load(std::string(TEST_DATA_DIR) + "/fff_print_tests/test_gcodewriter/config_lift_unlift.ini", ForwardCompatibilitySubstitutionRule::Disable);

        std::vector<unsigned int> extruder_ids {0};
        writer.set_extruders(extruder_ids);
        writer.set_extruder(0);

        WHEN("Z is set to 203") {
            double trouble_Z = 203;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                REQUIRE(writer.lazy_lift().size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.z_hop.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() emits gcode.") {
                            REQUIRE(writer.lazy_lift().size() > 0);
                        }
                    }
                }
            }
        }
        WHEN("Z is set to 500003") {
            double trouble_Z = 500003;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                REQUIRE(writer.lazy_lift().size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.z_hop.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() emits gcode.") {
                            REQUIRE(writer.lazy_lift().size() > 0);
                        }
                    }
                }
            }
        }
        WHEN("Z is set to 10.3") {
            double trouble_Z = 10.3;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                REQUIRE(writer.lazy_lift().size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.z_hop.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() emits gcode.") {
                            REQUIRE(writer.lazy_lift().size() > 0);
                        }
                    }
                }
            }
        }
		// The test above will fail for trouble_Z == 9007199254740992, where trouble_Z + 1.5 will be rounded to trouble_Z + 2.0 due to double mantisa overflow.
    }
}

SCENARIO("set_speed emits values with fixed-point output.", "[GCodeWriter]") {

    GIVEN("GCodeWriter instance") {
        GCodeWriter writer;
        WHEN("set_speed is called to set speed to 99999.123") {
            THEN("Output string is G1 F99999.123") {
                REQUIRE_THAT(writer.set_speed(99999.123), Catch::Matchers::Equals("G1 F99999.123\n"));
            }
        }
        WHEN("set_speed is called to set speed to 1") {
            THEN("Output string is G1 F1") {
                REQUIRE_THAT(writer.set_speed(1.0), Catch::Matchers::Equals("G1 F1\n"));
            }
        }
        WHEN("set_speed is called to set speed to 203.200022") {
            THEN("Output string is G1 F203.2") {
                REQUIRE_THAT(writer.set_speed(203.200022), Catch::Matchers::Equals("G1 F203.2\n"));
            }
        }
        WHEN("set_speed is called to set speed to 203.200522") {
            THEN("Output string is G1 F203.201") {
                REQUIRE_THAT(writer.set_speed(203.200522), Catch::Matchers::Equals("G1 F203.201\n"));
            }
        }
    }
}

TEST_CASE("DynaPin vertical layer changes preserve requested Z-hop", "[GCodeWriter][DynaPin][PinLift]")
{
    GCodeWriter writer;
    PrintConfig config;
    config.z_hop.values              = {0.2};
    config.retract_lift_above.values = {0.};
    config.retract_lift_below.values = {0.};
    writer.apply_print_config(config);
    writer.set_extruders({0});
    writer.set_extruder(0);
    writer.travel_to_z(10.);
    const bool eager = GENERATE(false, true);
    if (eager)
        writer.eager_lift(LiftType::NormalLift);
    else
        writer.lazy_lift(LiftType::NormalLift);
    writer.travel_to_z_preserving_lift(10.04);
    CHECK(writer.get_position().z() == Catch::Approx(10.24));
    CHECK(writer.get_zhop() == Catch::Approx(0.2));
    writer.travel_to_xy(Vec2d(5., 5.));
    CHECK(writer.get_position().z() == Catch::Approx(10.24));
    writer.unlift();
    CHECK(writer.get_position().z() == Catch::Approx(10.04));
    CHECK(writer.get_zhop() == 0.);
    writer.travel_to_z_preserving_lift(10.26);
    CHECK(writer.get_position().z() == Catch::Approx(10.26));
    CHECK(writer.get_zhop() == 0.);
}

TEST_CASE("Travel X component limit preserves Y speed and resets after protected motion", "[GCodeWriter][DynaPin][SupportSpeed]")
{
    GCodeWriter writer;
    PrintConfig config;
    config.travel_speed.value               = 150.;
    config.initial_layer_travel_speed.value = 120.;
    writer.apply_print_config(config);
    writer.set_extruders({0});
    writer.set_extruder(0);
    writer.set_is_first_layer(GENERATE(false, true));
    const bool xyz = GENERATE(false, true);
    auto move      = [&](const Vec3d& target) { return xyz ? writer.travel_to_xyz(target, "", true) : writer.travel_to_xy(to_2d(target)); };
    auto speed     = [&](const Vec3d& from, const Vec3d& to) {
        writer.set_position(from);
        writer.set_current_position_clear(true);
        GCodeReader reader;
        double      feedrate = 0.;
        reader.parse_buffer(move(to), [&](GCodeReader& state, const GCodeReader::GCodeLine& line) { feedrate = line.new_F(state) / 60.; });
        return feedrate;
    };
    const double normal_x = speed(Vec3d::Zero(), Vec3d(10., 0., 0.));
    const double normal_y = speed(Vec3d::Zero(), Vec3d(0., 10., 0.));
    writer.set_travel_max_x_speed(20.);
    CHECK_THAT(speed(Vec3d::Zero(), Vec3d(10., 0., 0.)), Catch::Matchers::WithinAbs(20., 0.001));
    CHECK_THAT(speed(Vec3d::Zero(), Vec3d(-0.05, 0., 0.)), Catch::Matchers::WithinAbs(20., 0.001));
    CHECK_THAT(speed(Vec3d::Zero(), Vec3d(0., 10., 0.)), Catch::Matchers::WithinAbs(normal_y, 0.001));
    CHECK_THAT(speed(Vec3d::Zero(), Vec3d(3., 4., 0.)), Catch::Matchers::WithinAbs(100. / 3., 0.001));
    writer.set_travel_max_x_speed(200.);
    CHECK_THAT(speed(Vec3d::Zero(), Vec3d(10., 0., 0.)), Catch::Matchers::WithinAbs(normal_x, 0.001));
    writer.set_travel_max_x_speed(20.);
    writer.set_travel_max_x_speed(0.);
    CHECK_THAT(speed(Vec3d::Zero(), Vec3d(10., 0., 0.)), Catch::Matchers::WithinAbs(normal_x, 0.001));
}
