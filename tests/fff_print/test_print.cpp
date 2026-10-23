#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/DynaPin.hpp"
#include "libslic3r/DynaPinLanding.hpp"
#include "libslic3r/DynaPinGCode.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include "libslic3r/DynaPinPlacement.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Support/TreeSupport.hpp"

#include <algorithm>
#include <cstdlib>
#include <future>
#include <chrono>
#include <iostream>
#include <random>
#include <tbb/task_arena.h>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "test_data.hpp"
#include "dynapin_selection_reference.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {
class PullConfigFile
{
public:
    const boost::filesystem::path path = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("dynapin-%%%%-%%%%.json");
    ~PullConfigFile() { boost::filesystem::remove(path); }
    void write(const nlohmann::json& config) const { std::ofstream(path.string()) << config.dump(); }
};

nlohmann::json pull_reference_fixtures()
{
    std::ifstream input(std::string(TEST_DATA_DIR) + "/dynapin/pull_gcode_reference.json");
    return nlohmann::json::parse(input);
}

void init_pull_config_print(Print& print, Model& model, const PullConfigFile& file)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"dynapin_config_path", file.path.string()}, {"printable_area", "0x0,180x0,180x180,0x180"}});
    print.apply(model, config);
}
} // namespace

TEST_CASE("DynaPin pull blocks match the Python reference", "[DynaPin][DynaPinPull]")
{
    PullConfigFile file;
    Print          print;
    Model          model;
    init_pull_config_print(print, model, file);
    for (const auto& fixture : pull_reference_fixtures()) {
        for (const auto& entry : fixture["cases"]) {
            auto json                                       = fixture["config"];
            json["pull_gcode"]["type"]                      = entry["type"];
            json["pull_gcode"]["alternating"]["row_parity"] = entry["row_parity"];
            file.write(json);
            DynaPin::Config config;
            std::string     error;
            INFO(entry.dump());
            REQUIRE(DynaPin::load_config_for_print(print, config, &error));
            const DynaPin::Pin pin{entry["pin"][0].get<int>(), entry["pin"][1].get<int>()};
            CHECK(DynaPin::pull_gcode_for_pin(config, pin) == entry["gcode"].get<std::string>());
            CHECK(DynaPin::pin_y(config, pin) == Catch::Approx(18. + pin.col * 12.4));
            const auto range = DynaPin::blocker_z_range(config, pin);
            CHECK(range.z_max == Catch::Approx(7.3 + pin.row * 7.4));
            CHECK(range.z_min == Catch::Approx(range.z_max - 5.));
        }
    }
}

TEST_CASE("DynaPin pull settings are isolated by mode", "[DynaPin][DynaPinPull]")
{
    auto json                                                = pull_reference_fixtures()[0]["config"];
    json["pull_gcode"]["normal"]["x_hook"]                   = 140.;
    json["pull_gcode"]["normal"]["x_front"]                  = 11.;
    json["pull_gcode"]["normal"]["y_origin_offset"]          = 20.;
    json["pull_gcode"]["alternating"]["normal"]["x_hook"]    = 150.;
    json["pull_gcode"]["alternating"]["normal"]["x_front"]   = 21.;
    json["pull_gcode"]["alternating"]["reverted"]["x_hook"]  = 160.;
    json["pull_gcode"]["alternating"]["reverted"]["x_front"] = 22.;
    json["pull_gcode"]["reverted"]["x_hook"]                 = 130.;
    json["pull_gcode"]["reverted"]["x_front"]                = 30.;

    PullConfigFile file;
    file.write(json);
    Print print;
    Model model;
    init_pull_config_print(print, model, file);
    DynaPin::Config config;
    REQUIRE(DynaPin::load_config_for_print(print, config));

    CHECK(DynaPin::support_x_front(config) == Catch::Approx(21.));
    CHECK(DynaPin::pull_gcode_for_pin(config, {1, 0}).find("G1 X150.0000 Z18.4000") != std::string::npos);
    CHECK(DynaPin::pull_gcode_for_pin(config, {0, 0}).find("G1 X160.0000 Z11.0000") != std::string::npos);
    config.pull_gcode.type = DynaPin::PullType::NormalOnly;
    CHECK(DynaPin::support_x_front(config) == Catch::Approx(11.));
    CHECK(DynaPin::pull_gcode_for_pin(config, {1, 0}).find("G1 X140.0000 Z13.4000") != std::string::npos);
    CHECK(DynaPin::pull_gcode_for_pin(config, {1, 0}).find("G1 Y26.5000") != std::string::npos);
    config.pull_gcode.type = DynaPin::PullType::RevertedOnly;
    CHECK(DynaPin::support_x_front(config) == Catch::Approx(30.));
    CHECK(DynaPin::pull_gcode_for_pin(config, {0, 0}).find("G1 X130.0000 Z6.0000") != std::string::npos);
}

TEST_CASE("DynaPin support speed settings default and validate", "[DynaPin][SupportSpeed]")
{
    PullConfigFile file;
    Print          print;
    Model          model;
    init_pull_config_print(print, model, file);
    auto json = pull_reference_fixtures()[0]["config"];
    file.write(json);
    DynaPin::Config defaults;
    REQUIRE(DynaPin::load_config_for_print(print, defaults));
    CHECK(defaults.support_speed.layers == 3);
    CHECK(defaults.support_speed.max_x_speed == 0.);
    // Existing JSON may still contain min_x_distance; it no longer gates the cap.
    json["support_speed"] = {{"layers", 2}, {"min_x_distance", 8.}, {"max_x_speed", 12.}};
    file.write(json);
    DynaPin::Config custom;
    REQUIRE(DynaPin::load_config_for_print(print, custom));
    CHECK(custom.support_speed.layers == 2);
    CHECK(custom.support_speed.max_x_speed == 12.);
    for (const auto& invalid : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json(true), nlohmann::json("3")}) {
        json["support_speed"]["layers"] = invalid;
        file.write(json);
        DynaPin::Config config;
        std::string     error;
        CHECK_FALSE(DynaPin::load_config_for_print(print, config, &error));
        CHECK_FALSE(error.empty());
    }
    json["support_speed"]["layers"] = 0;
    file.write(json);
    DynaPin::Config disabled;
    REQUIRE(DynaPin::load_config_for_print(print, disabled));
    CHECK(disabled.support_speed.layers == 0);
    for (const char* key : {"max_x_speed"}) {
        for (const auto& invalid : {nlohmann::json(-1.), nlohmann::json("nan"), nlohmann::json(true)}) {
            auto bad                  = json;
            bad["support_speed"][key] = invalid;
            file.write(bad);
            DynaPin::Config config;
            std::string     error;
            CHECK_FALSE(DynaPin::load_config_for_print(print, config, &error));
            CHECK_FALSE(error.empty());
        }
    }
}

TEST_CASE("DynaPin rejects legacy or invalid pull configurations", "[DynaPin][DynaPinPull]")
{
    const int invalid_case = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    auto      json         = pull_reference_fixtures()[0]["config"];
    auto&     pull         = json["pull_gcode"];
    switch (invalid_case) {
    case 0: pull = {{"x_hook", 169}, {"x_latch", 174}, {"x_front", 20}}; break;
    case 1: pull["type"] = "typo"; break;
    case 2: pull["alternating"]["row_parity"] = 2; break;
    case 3: pull["alternating"]["row_parity"] = true; break;
    case 4: pull["normal"]["feed_rate"] = 0; break;
    case 5: pull["alternating"]["normal"]["approach_z_offset"] = "nan"; break;
    case 6: pull["normal"].erase("x_front"); break;
    case 7: pull["reverted"]["y_offset"] = "-3.5junk"; break;
    }
    PullConfigFile file;
    file.write(json);
    Print print;
    Model model;
    init_pull_config_print(print, model, file);
    DynaPin::Config config;
    std::string     error;
    CHECK_FALSE(DynaPin::load_config_for_print(print, config, &error));
    CHECK_FALSE(error.empty());
}

namespace {

class ResourcesDirGuard
{
public:
    explicit ResourcesDirGuard(const std::string& path) : m_previous(resources_dir()) { set_resources_dir(path); }
    ~ResourcesDirGuard() { set_resources_dir(m_previous); }

private:
    std::string m_previous;
};

std::string test_resources_dir()
{
    return (boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() / "resources").string();
}

class DebugStageEnvGuard
{
public:
    DebugStageEnvGuard() : m_previous(std::getenv("DYNAPIN_DEBUG_STAGE")), m_had_previous(m_previous != nullptr)
    {
        if (m_had_previous)
            m_value = m_previous;
    }

    ~DebugStageEnvGuard()
    {
        if (m_had_previous)
            ::setenv("DYNAPIN_DEBUG_STAGE", m_value.c_str(), 1);
        else
            ::unsetenv("DYNAPIN_DEBUG_STAGE");
    }

private:
    const char *m_previous;
    bool        m_had_previous;
    std::string m_value;
};

class TemporaryDirGuard
{
public:
    TemporaryDirGuard() : m_previous(temporary_dir()) { set_temporary_dir(boost::filesystem::temp_directory_path().string()); }
    ~TemporaryDirGuard() { set_temporary_dir(m_previous); }

private:
    std::string m_previous;
};

class ImportedProjectDataGuard
{
public:
    ImportedProjectDataGuard(PlateDataPtrs& plate_data, std::vector<Preset*>& project_presets)
        : m_plate_data(plate_data), m_project_presets(project_presets)
    {
    }

    ~ImportedProjectDataGuard()
    {
        release_PlateData_list(m_plate_data);
        for (Preset* preset : m_project_presets)
            delete preset;
    }

private:
    PlateDataPtrs&       m_plate_data;
    std::vector<Preset*>& m_project_presets;
};

void init_dynapin_print(Print& print, Model& model, const DynamicPrintConfig& config)
{
    print.set_plate_origin(Vec3d::Zero());
    ModelObject* object = model.add_object();
    object->name        = "dynapin-test.stl";
    object->add_volume(Test::mesh(TestMesh::cube_20x20x20));
    ModelInstance* instance = object->add_instance();
    instance->set_offset({100., 100., 0.});
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
}

} // namespace

SCENARIO("PrintObject: Perimeter generation", "[PrintObject][.]") {
    GIVEN("20mm cube and default config") {
        WHEN("make_perimeters() is called")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, { { "fill_density", 0 } });
			const PrintObject &object = *print.objects().front();
			THEN("67 layers exist in the model") {
                REQUIRE(object.layers().size() == 66);
            }
            THEN("Every layer in region 0 has 1 island of perimeters") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.entities.size() == 1);
            }
            THEN("Every layer in region 0 has 3 paths in its perimeters list.") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.items_count() == 3);
            }
        }
    }
}

SCENARIO("Print: Skirt generation", "[Print][.]") {
    GIVEN("20mm cube and default config") {
        WHEN("Skirts is set to 2 loops")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
            	{ "skirt_height", 	1 },
        		{ "skirt_distance", 1 },
        		{ "skirts", 		2 }
            });
            THEN("Skirt Extrusion collection has 2 loops in it") {
                REQUIRE(print.skirt().items_count() == 2);
                REQUIRE(print.skirt().flatten().entities.size() == 2);
            }
        }
    }
}

SCENARIO("Print: Changing number of solid surfaces does not cause all surfaces to become internal.", "[Print][.]") {
    GIVEN("sliced 20mm cube and config with top_solid_surfaces = 2 and bottom_solid_surfaces = 1") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
		config.set_deserialize_strict({
			{ "top_solid_layers",		2 },
			{ "bottom_solid_layers",	1 },
			{ "layer_height",			0.25 }, // get a known number of layers
			{ "first_layer_height",		0.25 }
			});
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, config);
        // Precondition: Ensure that the model has 2 solid top layers (39, 38)
        // and one solid bottom layer (0).
		auto test_is_solid_infill = [&print](size_t obj_id, size_t layer_id) {
		    const Layer &layer = *(print.objects().at(obj_id)->get_layer((int)layer_id));
		    // iterate over all of the regions in the layer
		    for (const LayerRegion *region : layer.regions()) {
		        // for each region, iterate over the fill surfaces
		        for (const Surface &surface : region->fill_surfaces.surfaces)
		            CHECK(surface.is_solid());
		    }
		};
        print.process();
        test_is_solid_infill(0,  0); // should be solid
        test_is_solid_infill(0, 79); // should be solid
        test_is_solid_infill(0, 78); // should be solid
        WHEN("Model is re-sliced with top_solid_layers == 3") {
			config.set("top_solid_layers", 3);
			print.apply(model, config);
            print.process();
            THEN("Print object does not have 0 solid bottom layers.") {
                test_is_solid_infill(0, 0);
            }
            AND_THEN("Print object has 3 top solid layers") {
                test_is_solid_infill(0, 79);
                test_is_solid_infill(0, 78);
                test_is_solid_infill(0, 77);
            }
        }
    }
}

SCENARIO("Print: Moving a DynaPin model invalidates support material", "[Print][DynaPin][.]") {
    GIVEN("A processed model with DynaPin support optimization enabled") {
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_dynapin_support_optimization", true },
            { "enable_support",                    true }
        });
        init_dynapin_print(print, model, config);
        print.process();
        REQUIRE(print.is_step_done(posSupportMaterial));

        WHEN("The model is moved in the XY plane and applied again") {
            model.objects.front()->instances.front()->set_offset({110.0, 100.0, 0.0});
            print.apply(model, config);

            THEN("DynaPin support material is invalidated for regeneration") {
                REQUIRE_FALSE(print.is_step_done(posSupportMaterial));
            }
        }
    }
}

SCENARIO("Print: Rotating a DynaPin model invalidates slicing", "[Print][DynaPin]") {
    GIVEN("A processed model with DynaPin support optimization enabled") {
        ResourcesDirGuard resources_guard(test_resources_dir());
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_dynapin_support_optimization", true },
            { "enable_support",                    true },
            { "dynapin_config_path",               "Kingroon/dynapin/kp3s.json" },
            { "dynapin_selected_pins",              "1,0" }
        });
        init_dynapin_print(print, model, config);
        print.process();
        REQUIRE(print.is_step_done(posSlice));

        const Transform3d initial_trafo = print.objects().front()->trafo();

        WHEN("The model is rotated around the Z axis and applied again") {
            model.objects.front()->instances.front()->set_rotation(Z, Geometry::deg2rad(45.0));
            print.apply(model, config);

            THEN("The changed transform invalidates slicing") {
                REQUIRE(print.objects().size() == 1);
                CHECK_FALSE(print.objects().front()->trafo().matrix().isApprox(initial_trafo.matrix()));
                CHECK_FALSE(print.is_step_done(posSlice));

                print.process();
                CHECK(print.is_step_done(posSlice));
            }
        }
    }
}

TEST_CASE("DynaPin cached angle evaluation matches individual scene evaluations", "[Print][DynaPin][DynaPinPlacement]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());

    auto model = std::make_shared<Model>();
    TriangleMesh mesh = make_cube(5., 5., 10.);
    TriangleMesh overhang = make_cube(60., 20., 5.);
    overhang.translate(0., 0., 10.);
    mesh.merge(overhang);

    ModelObject *object = model->add_object();
    object->name = "dynapin-placement-cache.stl";
    object->add_volume(std::move(mesh));
    ModelInstance *target = object->add_instance();
    target->set_offset({50., 0., 0.});
    ModelInstance *other = object->add_instance(*target);
    other->set_offset({100., 100., 0.});

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"enable_dynapin_support_optimization", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", ""},
        {"support_type", "normal(auto)"},
        {"printable_area", "0x0,180x0,180x180,0x180"},
        {"printable_height", 180.},
    });

    DynaPin::PlacementSceneSnapshot scene;
    scene.model              = model;
    scene.config             = config;
    scene.target_object_id   = object->id();
    scene.target_instance_id = target->id();
    scene.initial_transform  = target->get_matrix();
    scene.world_center       = object->instance_bounding_box(*target, false).center();
    scene.printable_area     = {{0., 0.}, {180., 0.}, {180., 180.}, {0., 180.}};
    scene.printable_height   = 180.;
    scene.plate_origin       = {10., 20., 0.};

    const std::vector<DynaPin::PlacementCandidate> candidates{{90., 0.}, {90., 20.}, {90., 25.}, {90., 20.}};
    std::vector<std::optional<double>> flat_results;
    flat_results.reserve(candidates.size());
    for (const DynaPin::PlacementCandidate &candidate : candidates)
        flat_results.push_back(DynaPin::evaluate_scene_candidate(scene, candidate));

    std::vector<std::optional<double>> grouped_results;
    std::vector<size_t> completed;
    DynaPin::evaluate_scene_angle_group(
        scene, 90., candidates, grouped_results, [&completed](size_t index) { completed.push_back(index); });

    CHECK_FALSE(flat_results[0]);
    REQUIRE(grouped_results.size() == flat_results.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        CHECK(bool(grouped_results[i]) == bool(flat_results[i]));
        if (grouped_results[i] && flat_results[i]) {
            const double tolerance = std::max(1e-3, std::abs(*flat_results[i]) * 1e-6);
            CHECK(*grouped_results[i] == Catch::Approx(*flat_results[i]).margin(tolerance));
        }
    }
    CHECK(grouped_results[1] == grouped_results[3]);
    std::sort(completed.begin(), completed.end());
    CHECK(completed == std::vector<size_t>{0, 1, 2, 3});
}

TEST_CASE("DynaPin debug stage gates selection and support geometry", "[Print][DynaPin]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());

    for (const int stage : {0, 1, 2}) {
        Print print;
        Model model;
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"enable_dynapin_support_optimization", true},
            {"enable_support", true},
            {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
            {"dynapin_selected_pins", "1,0"},
            {"dynapin_debug_stage", stage},
        });
        init_dynapin_print(print, model, config);

        const auto &selection = print.dynapin_selection();
        const auto *object = print.objects().front();
        if (stage == 0) {
            CHECK(selection.pins.empty());
            CHECK(DynaPin::selected_blocker_boxes(print).empty());
            CHECK(DynaPin::pin_top_surfaces_for_object(*object).empty());
            CHECK(DynaPin::support_blocker_regions_local(*object).empty());
        } else {
            REQUIRE(selection.pins == std::vector<DynaPin::Pin>{{1, 0}});
            CHECK(DynaPin::selected_blocker_boxes(print).size() == 1);
            CHECK(DynaPin::pin_top_surfaces_for_object(*object).size() == 1);
            if (stage == 1)
                CHECK(DynaPin::support_blocker_regions_local(*object).empty());
            else
                CHECK(DynaPin::support_blocker_regions_local(*object).size() == 1);
        }
    }

    // The environment override is read for the same snapshot as the config;
    // restore the process environment before the test returns.
    DebugStageEnvGuard env_guard;
    REQUIRE(::setenv("DYNAPIN_DEBUG_STAGE", "0", 1) == 0);

    Print print;
    Model model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_dynapin_support_optimization", true},
        {"enable_support", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", "1,0"},
        {"dynapin_debug_stage", 2},
    });
    init_dynapin_print(print, model, config);
    CHECK(print.dynapin_selection().pins.empty());
    CHECK(DynaPin::selected_blocker_boxes(print).empty());
}

SCENARIO("Print: Empty DynaPin selection is resolved automatically without writeback", "[Print][DynaPin]") {
    GIVEN("Normal supports and a printer DynaPin candidate grid") {
        ResourcesDirGuard resources_guard(test_resources_dir());
        Print print;
        Model model;
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"enable_support", true},
            {"enable_dynapin_support_optimization", true},
            {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
            {"dynapin_selected_pins", ""},
            {"support_type", "normal(auto)"},
        });
        init_dynapin_print(print, model, config);
        print.process();

        THEN("The derived state is automatic and the setting remains empty") {
            CHECK(print.dynapin_selection().source == DynaPin::SelectionSource::Automatic);
            CHECK(print.config().dynapin_selected_pins.value.empty());
            CHECK(DynaPin::selected_blocker_boxes(print).size() == print.dynapin_selection().pins.size());
        }
    }
}

SCENARIO("Print: Empty DynaPin selection is evaluated for Tree supports", "[Print][DynaPin]")
{
    GIVEN("A floating slab with Tree supports and no manual pin list")
    {
        ResourcesDirGuard resources_guard(test_resources_dir());
        Print print;
        print.set_plate_origin(Vec3d::Zero());
        Model model;

        TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
        mesh.scale(Vec3f(1.f, 1.f, 0.25f));
        mesh.translate(30.f, 30.f, 0.f);
        TriangleMesh top = Test::mesh(TestMesh::cube_20x20x20);
        top.scale(Vec3f(0.1f, 0.1f, 0.1f));
        top.translate(11.5f, 11.5f, 23.f);
        mesh.merge(top);
        ModelObject* object = model.add_object();
        object->add_volume(std::move(mesh));
        ModelInstance* instance = object->add_instance();
        instance->set_offset({100., 0., 0.});
        object->ensure_on_bed();
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"enable_support", true},
            {"enable_dynapin_support_optimization", true},
            {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
            {"dynapin_selected_pins", ""},
            {"support_type", "tree(auto)"},
            {"support_style", "tree_slim"},
            {"support_interface_bottom_layers", 1},
        });
        print.auto_assign_extruders(object);
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();

        THEN("Automatic selection runs and Tree branches land above the bed")
        {
            CHECK(print.dynapin_selection().source == DynaPin::SelectionSource::Automatic);
            CHECK_FALSE(print.dynapin_selection().pins.empty());
            CHECK(print.dynapin_selection().warning.find("Tree or Organic") == std::string::npos);
            CHECK(print.config().dynapin_selected_pins.value.empty());
            REQUIRE_FALSE(print.objects().front()->support_layers().empty());
            const PrintObject* print_object = print.objects().front();
            const DynaPin::TreePinLandingPlan& landing_plan = print_object->dynapin_tree_landing_plan();
            REQUIRE_FALSE(landing_plan.landings.empty());
            CHECK(landing_plan.used_pins == print.dynapin_selection().pins);
            const std::vector<DynaPin::VirtualSupportSurface> landing_surfaces = DynaPin::pin_top_surfaces_for_object(*print_object);
            for (const DynaPin::TreePinLanding& landing : landing_plan.landings) {
                CHECK(landing.branch_id != 0);
                CHECK_FALSE(landing.footprint.empty());
                const auto surface_it = std::find_if(landing_surfaces.begin(), landing_surfaces.end(), [&landing](const DynaPin::VirtualSupportSurface& surface) {
                    return surface.pin == landing.pin;
                });
                REQUIRE(surface_it != landing_surfaces.end());
                CHECK(diff(Polygons{landing.footprint}, Polygons{surface_it->poly}).empty());
                const bool has_landing_support = std::any_of(print_object->support_layers().begin(), print_object->support_layers().end(), [&landing](const SupportLayer* layer) {
                    if (layer == nullptr || std::abs(layer->print_z - landing.support_print_z) > EPSILON ||
                        layer->bottom_z() + EPSILON < landing.print_z)
                        return false;
                    for (const ExtrusionEntity* entity : layer->support_fills.flatten().entities) {
                        const Polyline polyline = entity->as_polyline();
                        if (!intersection_ln(polyline.lines(), Polygons{landing.footprint}).empty())
                            return true;
                    }
                    return false;
                });
                CHECK(has_landing_support);
            }
            const std::vector<DynaPin::LocalBlocker> blockers = DynaPin::support_blocker_regions_local(*print_object);
            REQUIRE_FALSE(blockers.empty());
            double penetration_area = 0.;
            double extrusion_penetration_area = 0.;
            std::ostringstream penetrating_layers;
            for (const SupportLayer* layer : print_object->support_layers()) {
                if (layer == nullptr)
                    continue;
                Polygons support_polygons;
                polygons_append(support_polygons, to_polygons(layer->base_areas));
                for (const DynaPin::LocalBlocker& blocker : blockers) {
                    if (layer->bottom_z() + EPSILON < blocker.z_min || layer->print_z >= blocker.z_max - EPSILON)
                        continue;
                    const double overlap = area(intersection_ex(support_polygons, {blocker.poly}));
                    if (overlap > 0.) {
                        penetration_area += overlap;
                        penetrating_layers << layer->print_z << ':' << overlap * SCALING_FACTOR * SCALING_FACTOR << ' ';
                    }
                    for (const ExtrusionEntity* entity : layer->support_fills.flatten().entities)
                        extrusion_penetration_area += area(intersection_ex(entity->polygons_covered_by_width(), {blocker.poly}));
                }
            }
            INFO("support/blocker overlap by layer (z:mm2): " << penetrating_layers.str());
            CHECK(penetration_area < 1.);
            CHECK(extrusion_penetration_area < 1.);
            const bool landing_interface = std::any_of(print.objects().front()->support_layers().begin(),
                                                       print.objects().front()->support_layers().end(), [&landing_plan](const SupportLayer* layer) {
                                                           if (layer == nullptr || std::none_of(landing_plan.landings.begin(), landing_plan.landings.end(), [layer](const DynaPin::TreePinLanding& landing) {
                                                                   return std::abs(layer->print_z - landing.support_print_z) <= EPSILON;
                                                               }))
                                                               return false;
                                                           for (const ExtrusionEntity* entity : layer->support_fills.flatten().entities)
                                                               if (entity->role() == erSupportMaterialInterface)
                                                                   return true;
                                                           return false;
                                                       });
            CHECK(landing_interface);
            for (const int depth : {0, 1, 2, 3, -1}) {
                config.set_deserialize_strict({{"support_interface_bottom_layers", depth},
                                               {"support_interface_top_layers", 2},
                                               {"support_angle", 37.},
                                               {"support_interface_pattern", "grid"}});
                print.apply(model, config);
                print.process();
                size_t ordered_count = 0;
                for (const SupportLayer* layer : print.objects().front()->support_layers())
                    for (const auto* entity : layer->support_fills.entities) {
                        const auto* ordered = dynamic_cast<const ExtrusionEntityCollection*>(entity);
                        if (ordered == nullptr || !DynaPin::ordered_landing_collection(*ordered))
                            continue;
                        ++ordered_count;
                        coord_t previous_x = std::numeric_limits<coord_t>::lowest();
                        for (const auto& polyline : ordered->as_polylines())
                            for (const Point& point : polyline.points) {
                                CHECK(point.x() >= previous_x);
                                previous_x = point.x();
                            }
                    }
                CHECK(ordered_count > 0);
            }
        }
    }
}

TEST_CASE("DynaPin Tree selects pins after applying an optimized pose", "[Print][DynaPin][DynaPinPlacement]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    Print             print;
    Model             model;

    TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
    mesh.scale(Vec3f(1.f, 1.f, 0.25f));
    mesh.translate(30.f, 30.f, 0.f);
    TriangleMesh top = Test::mesh(TestMesh::cube_20x20x20);
    top.scale(Vec3f(0.1f, 0.1f, 0.1f));
    top.translate(11.5f, 11.5f, 23.f);
    mesh.merge(top);
    ModelObject* object = model.add_object();
    object->add_volume(std::move(mesh));
    ModelInstance* instance = object->add_instance();
    instance->set_offset({100., 0., 0.});
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"enable_dynapin_support_optimization", true},
        {"enable_dynapin_placement_optimization", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", ""},
        {"support_type", "tree(auto)"},
        {"support_style", "tree_slim"},
        {"support_interface_bottom_layers", 1},
        {"printable_area", "0x0,180x0,180x180,0x180"},
        {"printable_height", 180.},
    });
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();

    const DynaPin::PlacementPreparationResult preparation = DynaPin::prepare_placement_task(model, print,
                                                                                            {{0., 0.}, {180., 0.}, {180., 180.}, {0., 180.}},
                                                                                            180., {}, 0);
    REQUIRE(preparation.task);
    CHECK(DynaPin::evaluate_scene_candidate(preparation.task->scene, {90., 0.}).has_value());
    CHECK_FALSE(DynaPin::evaluate_scene_candidate(preparation.task->scene, {90., 20.}).has_value());
    std::vector<std::optional<double>> grouped_bad_pose;
    DynaPin::evaluate_scene_angle_group(preparation.task->scene, 90., {{90., 20.}}, grouped_bad_pose);
    REQUIRE(grouped_bad_pose.size() == 1);
    CHECK_FALSE(grouped_bad_pose.front().has_value());

    DynaPin::PlacementResult optimized_pose;
    optimized_pose.status    = DynaPin::PlacementStatus::Improved;
    optimized_pose.candidate = {90., 0.};
    REQUIRE(DynaPin::apply_placement_result(model, *preparation.task, optimized_pose).applied);

    print.apply(model, config);
    print.validate();
    print.process();

    REQUIRE(print.dynapin_selection().source == DynaPin::SelectionSource::Automatic);
    REQUIRE_FALSE(print.dynapin_selection().pins.empty());
    const std::vector<DynaPin::Pin> expected_pins{{1, 5}, {1, 6}, {1, 7}};
    CHECK(print.dynapin_selection().pins == expected_pins);
    const PrintObject* print_object = print.objects().front();
    REQUIRE_FALSE(print_object->support_layers().empty());
    const DynaPin::TreePinLandingPlan& landing_plan = print_object->dynapin_tree_landing_plan();
    REQUIRE_FALSE(landing_plan.landings.empty());
    CHECK(landing_plan.used_pins == print.dynapin_selection().pins);

    const std::vector<DynaPin::VirtualSupportSurface> landing_surfaces = DynaPin::pin_top_surfaces_for_object(*print_object);
    const bool                                        has_landing_support =
        std::any_of(landing_plan.landings.begin(), landing_plan.landings.end(),
                    [&print_object, &landing_surfaces](const DynaPin::TreePinLanding& landing) {
                        const auto surface_it = std::find_if(landing_surfaces.begin(), landing_surfaces.end(),
                                                             [&landing](const DynaPin::VirtualSupportSurface& surface) {
                                                                 return surface.pin == landing.pin;
                                                             });
                        if (surface_it == landing_surfaces.end() || !diff(Polygons{landing.footprint}, Polygons{surface_it->poly}).empty())
                            return false;
                        return std::any_of(print_object->support_layers().begin(), print_object->support_layers().end(),
                                           [&landing](const SupportLayer* layer) {
                                               if (layer == nullptr || std::abs(layer->print_z - landing.support_print_z) > EPSILON ||
                                                   layer->bottom_z() + EPSILON < landing.print_z)
                                                   return false;
                                               for (const ExtrusionEntity* entity : layer->support_fills.flatten().entities)
                                                   if (!intersection_ln(entity->as_polyline().lines(), Polygons{landing.footprint}).empty())
                                                       return true;
                                               return false;
                                           });
                    });
    CHECK(has_landing_support);
}

TEST_CASE("DynaPin Tree placement rejects candidates without projected pins", "[Print][DynaPin][DynaPinPlacement]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    Print print;
    Model model;
    ModelObject* object = model.add_object();
    object->add_volume(Test::mesh(TestMesh::cube_20x20x20));
    ModelInstance* instance = object->add_instance();
    instance->set_offset({80., 80., 0.});
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"enable_dynapin_support_optimization", true},
        {"enable_dynapin_placement_optimization", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", ""},
        {"support_type", "tree(auto)"},
        {"support_style", "tree_slim"},
    });
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();

    const DynaPin::PlacementPreparationResult preparation = DynaPin::prepare_placement_task(
        model, print, {{0., 0.}, {180., 0.}, {180., 180.}, {0., 180.}}, 180., {}, 0);
    REQUIRE(preparation.task);
    CHECK_FALSE(DynaPin::evaluate_scene_candidate(preparation.task->scene, {0., 0.}).has_value());
}

TEST_CASE("DynaPin Tree bounded placement keeps Simple Bridge support pins", "[Print][DynaPin][DynaPinPlacement]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    DebugStageEnvGuard env_guard;
    TemporaryDirGuard temp_dir_guard;
    REQUIRE(::setenv("DYNAPIN_DEBUG_STAGE", "2", 1) == 0);

    Model model;
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitution{ForwardCompatibilitySubstitutionRule::Disable};
    const std::string fixture_path = (boost::filesystem::path(TEST_DATA_DIR) / "dynapin" / "Simple_Bridge.3mf").string();
    PlateDataPtrs plate_data;
    std::vector<Preset*> project_presets;
    ImportedProjectDataGuard imported_project_data_guard(plate_data, project_presets);
    bool is_bbl_3mf = false;
    Semver file_version;
    REQUIRE(load_bbs_3mf(fixture_path.c_str(), &config, &substitution, &model, &plate_data, &project_presets,
                         &is_bbl_3mf, &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(model.objects.size() == 1);

    // Use the printer, filament, layer, and support settings embedded in the
    // project; only switch the support path to automatic Tree placement.
    config.set_deserialize_strict({
        {"enable_dynapin_support_optimization", true},
        {"enable_dynapin_placement_optimization", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_debug_stage", 2},
        {"dynapin_selected_pins", ""},
        {"support_type", "tree(auto)"},
        {"printable_area", "0x0,180x0,180x180,0x180"},
        {"printable_height", 180.},
    });

    Print print;
    print.set_plate_origin(Vec3d::Zero());
    for (ModelObject* object : model.objects)
        print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();

    const std::vector<Vec2d> printable_area{{0., 0.}, {180., 0.}, {180., 180.}, {0., 180.}};
    DynaPin::PlacementPreparationResult preparation =
        DynaPin::prepare_placement_task(model, print, printable_area, 180., {}, 0);
    REQUIRE(preparation.task);
    DynaPin::PlacementTask task = std::move(*preparation.task);
    // Keep integration tests independent of host memory pressure.
    task.search.angle_group_concurrency = 1;

    // The reported production winner is 355 degrees relative to the loaded
    // 90-degree model pose. Limit coarse and local candidates to the current
    // pose and that winner so this integration test avoids the full 360-degree
    // placement sweep while still exercising run_placement_task().
    task.search.delta_y_range_for_rotation = [](double angle) -> std::optional<DynaPin::DeltaYInterval> {
        const double normalized = DynaPin::normalized_rotation_deg(angle);
        return normalized == 0. || normalized == 355. ? std::optional<DynaPin::DeltaYInterval>{{0., 0.}} : std::nullopt;
    };
    const DynaPin::AngleGroupEvaluator scene_evaluator = DynaPin::make_scene_angle_evaluator(task.scene);
    task.search.angle_evaluator = [scene_evaluator](double angle, const std::vector<DynaPin::PlacementCandidate>& candidates,
                                                    std::vector<std::optional<double>>& results,
                                                    const DynaPin::CandidateCompletedCallback& completed) {
        const double normalized = DynaPin::normalized_rotation_deg(angle);
        if (normalized != 0. && normalized != 355.) {
            results.assign(candidates.size(), std::nullopt);
            return;
        }
        scene_evaluator(angle, candidates, results, completed);
    };

    const DynaPin::PlacementResult result = DynaPin::run_placement_task(task);
    INFO("placement status=" << static_cast<int>(result.status) << ", rotation=" << result.candidate.rotation_deg
         << ", delta_y=" << result.candidate.delta_y << ", volume=" << result.volume_mm3);
    REQUIRE(result.status == DynaPin::PlacementStatus::Improved);
    CHECK(result.candidate.rotation_deg == Catch::Approx(355.));
    CHECK(result.candidate.delta_y == Catch::Approx(0.));
    REQUIRE(DynaPin::apply_placement_result(model, task, result).applied);

    print.apply(model, config);
    print.validate();
    print.process();

    REQUIRE(print.dynapin_selection().source == DynaPin::SelectionSource::Automatic);
    REQUIRE_FALSE(print.dynapin_selection().pins.empty());
    const std::vector<DynaPin::Pin> expected_pins{{1, 5}, {1, 6}, {1, 7}};
    CHECK(print.dynapin_selection().pins == expected_pins);
    const PrintObject* print_object = print.objects().front();
    const DynaPin::TreePinLandingPlan& landing_plan = print_object->dynapin_tree_landing_plan();
    REQUIRE_FALSE(landing_plan.landings.empty());
    CHECK(landing_plan.used_pins == print.dynapin_selection().pins);
    const bool has_landing_support = std::any_of(
        landing_plan.landings.begin(), landing_plan.landings.end(), [&print_object](const DynaPin::TreePinLanding& landing) {
            return std::any_of(print_object->support_layers().begin(), print_object->support_layers().end(),
                               [&landing](const SupportLayer* layer) {
                                   if (layer == nullptr || std::abs(layer->print_z - landing.support_print_z) > EPSILON ||
                                       layer->bottom_z() + EPSILON < landing.print_z)
                                       return false;
                                   for (const ExtrusionEntity* entity : layer->support_fills.flatten().entities)
                                       if (!intersection_ln(entity->as_polyline().lines(), Polygons{landing.footprint}).empty())
                                           return true;
                                   return false;
                               });
        });
    REQUIRE(has_landing_support);

    const double dynapin_support_volume = DynaPin::support_volume_mm3(print);
    Model disabled_model(model);
    DynamicPrintConfig disabled_config = config;
    disabled_config.set_deserialize_strict({
        {"enable_dynapin_support_optimization", false},
        {"enable_dynapin_placement_optimization", false},
    });
    Print disabled_print;
    disabled_print.set_plate_origin(Vec3d::Zero());
    for (ModelObject* object : disabled_model.objects)
        disabled_print.auto_assign_extruders(object);
    disabled_print.apply(disabled_model, disabled_config);
    disabled_print.validate();
    disabled_print.process();
    const double disabled_support_volume = DynaPin::support_volume_mm3(disabled_print);
    INFO("support volume with DynaPin=" << dynapin_support_volume << " mm3, disabled=" << disabled_support_volume << " mm3");
    CHECK(dynapin_support_volume < disabled_support_volume * 0.9);
}

TEST_CASE("DynaPin Tree placement optimization reslices with selected pins", "[Print][DynaPin][DynaPinPlacement][.]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    Model model;
    TriangleMesh bridge = Test::mesh(TestMesh::cube_20x20x20);
    TriangleMesh far_leg = Test::mesh(TestMesh::cube_20x20x20);
    far_leg.translate(0.f, 60.f, 0.f);
    bridge.merge(far_leg);
    TriangleMesh deck = Test::mesh(TestMesh::cube_20x20x20);
    deck.scale(Vec3f(1.f, 4.f, 1.f));
    deck.translate(0.f, 0.f, 20.f);
    bridge.merge(deck);
    ModelObject* object = model.add_object();
    object->add_volume(std::move(bridge));
    ModelInstance* instance = object->add_instance();
    instance->set_offset({80., 55., 0.});
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"enable_dynapin_support_optimization", true},
        {"enable_dynapin_placement_optimization", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", ""},
        {"support_type", "tree(auto)"},
        {"support_style", "tree_slim"},
        {"support_interface_bottom_layers", 1},
        {"printable_area", "0x0,180x0,180x180,0x180"},
        {"printable_height", 180.},
    });
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();

    auto preparation = DynaPin::prepare_placement_task(model, print,
        {{0., 0.}, {180., 0.}, {180., 180.}, {0., 180.}}, 180., {}, 0);
    REQUIRE(preparation.task);
    DynaPin::PlacementTask task = std::move(*preparation.task);
    // Keep integration tests independent of host memory pressure.
    task.search.angle_group_concurrency    = 1;
    task.search.delta_y_range_for_rotation = [](double angle) -> std::optional<DynaPin::DeltaYInterval> {
        return angle == 0. ? std::optional<DynaPin::DeltaYInterval>{{-10., -10.}} : std::nullopt;
    };
    const DynaPin::PlacementResult result = DynaPin::run_placement_task(task);
    INFO("placement status=" << static_cast<int>(result.status) << ", rotation=" << result.candidate.rotation_deg
         << ", delta_y=" << result.candidate.delta_y << ", volume=" << result.volume_mm3);
    REQUIRE(result.status == DynaPin::PlacementStatus::Improved);
    REQUIRE(DynaPin::apply_placement_result(model, task, result).applied);

    print.apply(model, config);
    print.validate();
    print.process();
    REQUIRE(print.dynapin_selection().source == DynaPin::SelectionSource::Automatic);
    REQUIRE_FALSE(print.dynapin_selection().pins.empty());
    REQUIRE(print.objects().front()->dynapin_tree_landing_plan().used_pins == print.dynapin_selection().pins);
    const auto& support_layers = print.objects().front()->support_layers();
    CHECK(std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer* layer) {
        return layer != nullptr && !layer->support_fills.flatten().entities.empty();
    }));
}

TEST_CASE("Tree support generation works with DynaPin disabled", "[Print][DynaPin]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    Print             print;
    Model             model;

    TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
    mesh.scale(Vec3f(1.f, 1.f, 0.25f));
    mesh.translate(30.f, 30.f, 0.f);
    TriangleMesh top = Test::mesh(TestMesh::cube_20x20x20);
    top.scale(Vec3f(0.1f, 0.1f, 0.1f));
    top.translate(11.5f, 11.5f, 23.f);
    mesh.merge(top);
    ModelObject* object = model.add_object();
    object->add_volume(std::move(mesh));
    ModelInstance* instance = object->add_instance();
    instance->set_offset({100., 0., 0.});
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"enable_dynapin_support_optimization", false},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", ""},
        {"support_type", "tree(auto)"},
        {"support_style", "tree_slim"},
        {"support_interface_bottom_layers", 1},
    });
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    REQUIRE_FALSE(print.objects().front()->support_layers().empty());
    CHECK(print.dynapin_selection().pins.empty());
}

TEST_CASE("DynaPin Tree rejects pins when a wide overhang has unlandable branches", "[DynaPin][Print]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    Print print;
    Model model;
    TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
    mesh.scale(Vec3f(1.f, 1.f, 0.25f));
    mesh.translate(30.f, 30.f, 0.f);
    TriangleMesh top = Test::mesh(TestMesh::cube_20x20x20);
    top.scale(Vec3f(1.f, 1.f, 0.1f));
    top.translate(0.f, 0.f, 23.f);
    mesh.merge(top);
    ModelObject* object = model.add_object();
    object->add_volume(std::move(mesh));
    ModelInstance* instance = object->add_instance();
    instance->set_offset({100., 0., 0.});
    object->ensure_on_bed();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"enable_dynapin_support_optimization", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", ""},
        {"support_type", "tree(auto)"},
        {"support_style", "tree_slim"},
    });
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    int cancel_calls = 0;
    print.set_cancel_callback([&cancel_calls]() { ++cancel_calls; });
    print.process();

    CHECK(cancel_calls == 0);
    CHECK(print.dynapin_selection().source == DynaPin::SelectionSource::Automatic);
    CHECK(print.dynapin_selection().pins.empty());
    CHECK(print.objects().front()->dynapin_tree_landing_plan().landings.empty());
    CHECK_FALSE(print.objects().front()->support_layers().empty());
}

TEST_CASE("DynaPin Tree keeps all three Simple Bridge pins", "[DynaPin][Print]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    Model model;
    TriangleMesh bridge = Test::mesh(TestMesh::cube_20x20x20);
    TriangleMesh far_leg = Test::mesh(TestMesh::cube_20x20x20);
    far_leg.translate(0.f, 60.f, 0.f);
    bridge.merge(far_leg);
    TriangleMesh deck = Test::mesh(TestMesh::cube_20x20x20);
    deck.scale(Vec3f(1.f, 4.f, 1.f));
    deck.translate(0.f, 0.f, 20.f);
    bridge.merge(deck);
    ModelObject* object = model.add_object();
    object->add_volume(std::move(bridge));
    ModelInstance* instance = object->add_instance();
    instance->set_offset({80., 45., 0.});
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"enable_dynapin_support_optimization", true},
        {"enable_dynapin_placement_optimization", false},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", ""},
        {"support_type", "tree(auto)"},
        {"support_style", "tree_slim"},
        {"layer_height", 0.3},
        {"support_top_z_distance", 0.2},
        {"support_bottom_z_distance", 0.2},
        {"support_interface_top_layers", 2},
        {"support_interface_bottom_layers", 2},
        {"support_line_width", 0.44},
        {"tree_support_branch_angle", 45},
        {"tree_support_branch_diameter", 5},
        {"printable_area", "0x0,180x0,180x180,0x180"},
    });

    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    const std::vector<DynaPin::Pin> expected{{1, 5}, {1, 6}, {1, 7}};
    REQUIRE(print.dynapin_selection().pins == expected);
    REQUIRE(print.objects().front()->dynapin_tree_landing_plan().used_pins == expected);
}

TEST_CASE("DynaPin Tree uses a manually selected pin as a landing", "[DynaPin][Print]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    const int         bottom_layers = GENERATE(0, 1, 2, 3, -1);
    const float       top_z         = GENERATE(23.f, 28.f);
    for (const char* support_style : {"tree_slim", "tree_strong", "tree_hybrid"}) {
        CAPTURE(support_style, bottom_layers, top_z);
        Print        print;
        Model        model;
        TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
        mesh.scale(Vec3f(1.f, 1.f, 0.25f));
        mesh.translate(30.f, 30.f, 0.f);
        TriangleMesh top = Test::mesh(TestMesh::cube_20x20x20);
        top.scale(Vec3f(0.1f, 0.1f, 0.1f));
        top.translate(11.5f, 11.5f, top_z);
        mesh.merge(top);
        ModelObject* object = model.add_object();
        object->add_volume(std::move(mesh));
        ModelInstance* instance = object->add_instance();
        instance->set_offset({100., 0., 0.});
        object->ensure_on_bed();
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"enable_support", true},
            {"enable_dynapin_support_optimization", true},
            {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
            {"dynapin_selected_pins", "2,0"},
            {"support_type", "tree(auto)"},
            {"support_style", support_style},
            {"support_interface_top_layers", 2},
            {"support_interface_bottom_layers", bottom_layers},
        });
        print.auto_assign_extruders(object);
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();

        CHECK(print.dynapin_selection().source == DynaPin::SelectionSource::Manual);
        const std::vector<DynaPin::Pin> expected_pins{{2, 0}};
        REQUIRE(print.dynapin_selection().pins == expected_pins);
        const DynaPin::TreePinLandingPlan& plan = print.objects().front()->dynapin_tree_landing_plan();
        CHECK_FALSE(plan.landings.empty());
        CHECK(plan.used_pins == print.dynapin_selection().pins);
        size_t              landing_layers = 0;
        double              first_bottom   = std::numeric_limits<double>::max();
        std::vector<double> available_z;
        for (const SupportLayer* layer : print.objects().front()->support_layers()) {
            if (layer->print_z > 22.1 && layer->has_extrusions())
                available_z.push_back(layer->print_z);
            bool       has_landing = false;
            const auto flat        = layer->support_fills.flatten();
            auto       check_height = [&](const ExtrusionPath& path) {
                if (layer->print_z > 22.1 + EPSILON)
                    first_bottom = std::min(first_bottom, layer->print_z - path.height);
            };
            for (const ExtrusionEntity* entity : flat.entities) {
                if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity)) {
                    has_landing |= path->is_dynapin_landing;
                    check_height(*path);
                } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
                    has_landing |= std::any_of(multipath->paths.begin(), multipath->paths.end(),
                                               [](const auto& path) { return path.is_dynapin_landing; });
                    for (const auto& path : multipath->paths)
                        check_height(path);
                } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
                    has_landing |= std::any_of(loop->paths.begin(), loop->paths.end(),
                                               [](const auto& path) { return path.is_dynapin_landing; });
                    for (const auto& path : loop->paths)
                        check_height(path);
                }
            }
            landing_layers += has_landing;
        }
        CAPTURE(available_z);
        if (top_z == 23.f)
            REQUIRE(available_z.size() >= 2);
        else
            REQUIRE(available_z.size() >= 3);
        CHECK(landing_layers == 1);
        CHECK(first_bottom == Catch::Approx(22.1).margin(EPSILON));
    }
}

TEST_CASE("DynaPin Tree landing targets the middle of a pin edge", "[DynaPin][Print]")
{
    Polygon safe_region;
    safe_region.points                = {Point(scale_(0.), scale_(0.)), Point(scale_(40.), scale_(0.)), Point(scale_(40.), scale_(4.)),
                                         Point(scale_(0.), scale_(4.))};
    const std::optional<Point> target = DynaPin::nearest_pin_landing_point(safe_region, Point(scale_(20.), scale_(8.)));
    REQUIRE(target);
    CHECK(std::abs(unscale_(target->x()) - 20.) < 0.01);
    CHECK(std::abs(unscale_(target->y()) - 4.) < 0.01);
    CHECK(*target != safe_region.points[2]);
    CHECK(*target != safe_region.points[3]);
    const std::optional<Point> corner_target = DynaPin::nearest_pin_landing_point(safe_region, Point(scale_(-2.), scale_(-2.)));
    REQUIRE(corner_target);
    CHECK(*corner_target != safe_region.points[0]);
}

SCENARIO("Print: Default Tree style switches to a DynaPin-capable generator", "[Print][DynaPin]")
{
    GIVEN("Tree support with the default style and automatic pin selection")
    {
        ResourcesDirGuard  resources_guard(test_resources_dir());
        Print              print;
        Model              model;
        TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
        mesh.scale(Vec3f(1.f, 1.f, 0.25f));
        mesh.translate(30.f, 30.f, 0.f);
        TriangleMesh top = Test::mesh(TestMesh::cube_20x20x20);
        top.scale(Vec3f(0.1f, 0.1f, 0.1f));
        top.translate(11.5f, 11.5f, 23.f);
        mesh.merge(top);
        ModelObject* object = model.add_object();
        object->add_volume(std::move(mesh));
        ModelInstance* instance = object->add_instance();
        instance->set_offset({100., 0., 0.});
        object->ensure_on_bed();

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"enable_support", true},
            {"enable_dynapin_support_optimization", true},
            {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
            {"dynapin_selected_pins", ""},
            {"support_type", "tree(auto)"},
            {"support_style", "default"},
        });
        print.auto_assign_extruders(object);
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();

        THEN("Pins are selected and the normal Tree generator is used")
        {
            CHECK_FALSE(print.dynapin_selection().pins.empty());
            CHECK(print.dynapin_selection().warning.find("Organic") == std::string::npos);
            REQUIRE_FALSE(print.objects().front()->support_layers().empty());
            CHECK(print.objects().front()->support_layers().front()->support_type == stInnerTree);
        }
    }
}

SCENARIO("Print: Explicit Organic Tree remains outside DynaPin support", "[Print][DynaPin]")
{
    GIVEN("Organic Tree support and automatic pin selection")
    {
        ResourcesDirGuard resources_guard(test_resources_dir());
        Print             print;
        Model             model;
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"enable_support", true},
            {"enable_dynapin_support_optimization", true},
            {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
            {"dynapin_selected_pins", ""},
            {"support_type", "tree(auto)"},
            {"support_style", "organic"},
        });
        init_dynapin_print(print, model, config);
        print.process();

        CHECK(print.dynapin_selection().pins.empty());
        CHECK(print.dynapin_selection().warning.find("Organic") != std::string::npos);
    }
}

SCENARIO("Print: Automatic DynaPin selection starts at the model overhang", "[Print][DynaPin]") {
    GIVEN("A floating top slab above a support base and an empty pin list") {
        ResourcesDirGuard resources_guard(test_resources_dir());
        Print print;
        Model model;

        // Keep the base outside the pin landing region while the upper slab
        // overlaps it.  This makes the expected pin safe in its own blocker
        // Z range and forces the detector to use the slab's model-layer Z,
        // rather than the lower support contact-layer Z.
        TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
        mesh.scale(Vec3f(1.f, 1.f, 0.25f));
        mesh.translate(30.f, 30.f, 0.f);
        TriangleMesh top = Test::mesh(TestMesh::cube_20x20x20);
        top.scale(Vec3f(1.f, 1.f, 0.1f));
        top.translate(0.f, 0.f, 23.f);
        mesh.merge(top);

        ModelObject *object = model.add_object();
        object->add_volume(std::move(mesh));
        ModelInstance *instance = object->add_instance();
        instance->set_offset({100., 0., 0.});
        object->ensure_on_bed();

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"enable_support", true},
            {"enable_dynapin_support_optimization", true},
            {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
            {"dynapin_selected_pins", ""},
            {"support_type", "normal(auto)"},
        });
        print.auto_assign_extruders(object);
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();

        THEN("A pin at the slab height is selected automatically") {
            REQUIRE(print.dynapin_selection().source == DynaPin::SelectionSource::Automatic);
            CHECK(print.dynapin_selection().pins == std::vector<DynaPin::Pin>{{2, 0}, {2, 1}});
            CHECK(std::any_of(print.dynapin_selection().pins.begin(), print.dynapin_selection().pins.end(),
                              [](const DynaPin::Pin &pin) { return pin.row == 2; }));
            CHECK(print.config().dynapin_selected_pins.value.empty());
        }
    }
}

SCENARIO("Print: Automatic DynaPin selection keeps independent lower overhangs", "[Print][DynaPin]") {
    GIVEN("Two stacked floating overhangs and an empty pin list") {
        ResourcesDirGuard resources_guard(test_resources_dir());
        Print print;
        Model model;

        // Keep a bed-connected base outside the pin landing area, then add two
        // separate overhang stages in the same XY region.  The lower stage is
        // above the row=2 pin top, while the upper stage is above the row=4
        // pin top.
        TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
        mesh.scale(Vec3f(1.f, 1.f, 0.25f));
        mesh.translate(30.f, 150.f, 0.f);
        TriangleMesh lower = Test::mesh(TestMesh::cube_20x20x20);
        lower.scale(Vec3f(1.f, 1.f, 0.1f));
        lower.translate(0.f, 0.f, 23.f);
        mesh.merge(lower);
        TriangleMesh upper = Test::mesh(TestMesh::cube_20x20x20);
        upper.scale(Vec3f(1.f, 1.f, 0.1f));
        upper.translate(0.f, 0.f, 39.f);
        mesh.merge(upper);

        ModelObject *object = model.add_object();
        object->add_volume(std::move(mesh));
        ModelInstance *instance = object->add_instance();
        instance->set_offset({100., 0., 0.});
        object->ensure_on_bed();

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"enable_support", true},
            {"enable_dynapin_support_optimization", true},
            {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
            {"dynapin_selected_pins", ""},
            {"support_type", "normal(auto)"},
            {"printable_area", "0x0,180x0,180x180,0x180"},
        });
        print.auto_assign_extruders(object);
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();

        THEN("Both overhang stages select pins at their respective heights") {
            REQUIRE(print.dynapin_selection().source == DynaPin::SelectionSource::Automatic);
            CHECK(print.dynapin_selection().pins == std::vector<DynaPin::Pin>{{2, 0}, {2, 1}, {4, 0}, {4, 1}});
            CHECK(std::any_of(print.dynapin_selection().pins.begin(), print.dynapin_selection().pins.end(),
                              [](const DynaPin::Pin &pin) { return pin.row == 2; }));
            CHECK(std::any_of(print.dynapin_selection().pins.begin(), print.dynapin_selection().pins.end(),
                              [](const DynaPin::Pin &pin) { return pin.row == 4; }));
        }

        THEN("Each selected pin has a blocker") {
            const std::vector<DynaPin::BlockerBox> boxes = DynaPin::selected_blocker_boxes(print);
            REQUIRE(boxes.size() == print.dynapin_selection().pins.size());
        }
    }
}

SCENARIO("Print: Automatic DynaPin pin collision check evaluates all model instances", "[Print][DynaPin]") {
    GIVEN("A printer DynaPin config and multiple model instances") {
        ResourcesDirGuard resources_guard(test_resources_dir());
        Print print;
        Model model;
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"enable_support", true},
            {"enable_dynapin_support_optimization", true},
            {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
            {"dynapin_selected_pins", ""},
            {"support_type", "normal(auto)"},
        });
        init_dynapin_print(print, model, config);

        DynaPin::Config dynapin_config;
        REQUIRE(DynaPin::load_config_for_print(print, dynapin_config));

        THEN("Collision checking properly identifies collisions across all instances") {
            const DynaPin::Pin test_pin{0, 1};
            const bool collides = DynaPin::pin_collides_with_model(print, dynapin_config, test_pin);
            CHECK_FALSE(collides);
        }
    }
}

SCENARIO("Print: Model geometry before x_front does not collide with a DynaPin blocker", "[Print][DynaPin]") {
    GIVEN("A model entirely before the pull path") {
        ResourcesDirGuard resources_guard(test_resources_dir());
        Print print;
        Model             model;
        TriangleMesh      mesh = make_cube(10., 10., 5.);
        mesh.translate(5.f, 5.f, 0.f);
        ModelObject *object = model.add_object();
        object->name        = "dynapin-before-x-front-test.stl";
        object->add_volume(std::move(mesh));
        object->add_instance();
        object->ensure_on_bed();

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"enable_support", true},
            {"enable_dynapin_support_optimization", true},
            {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
            {"dynapin_selected_pins", ""},
            {"dynapin_debug_stage", 0},
            {"support_type", "normal(auto)"},
        });
        print.auto_assign_extruders(object);
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();

        DynaPin::Config dynapin_config;
        REQUIRE(DynaPin::load_config_for_print(print, dynapin_config));
        CHECK(DynaPin::support_x_front(dynapin_config) == Catch::Approx(20.));
        CHECK_FALSE(DynaPin::pin_collides_with_model(print, dynapin_config, {0, 0}));
    }
}

SCENARIO("Print: Automatic DynaPin collision check covers the full pull path", "[Print][DynaPin]") {
    GIVEN("A model inside the blocker pull-path range") {
        ResourcesDirGuard resources_guard(test_resources_dir());
        Print print;
        Model model;

        TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
        mesh.translate(40.f, 5.f, 0.f);
        ModelObject *object = model.add_object();
        object->add_volume(std::move(mesh));
        object->add_instance();
        object->ensure_on_bed();

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"enable_support", true},
            {"enable_dynapin_support_optimization", true},
            {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
            {"dynapin_selected_pins", ""},
            {"support_type", "normal(auto)"},
        });
        print.auto_assign_extruders(object);
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        std::vector<DynaPin::SupportSlab> slabs;
        REQUIRE(print.generate_support_geometry_only(slabs, {}));

        DynaPin::Config dynapin_config;
        REQUIRE(DynaPin::load_config_for_print(print, dynapin_config));

        THEN("The blocker collision is handled without rejecting the candidate Print") {
            CHECK(DynaPin::pin_collides_with_model(print, dynapin_config, {0, 0}));
        }
    }
}

SCENARIO("Print: DynaPin controls copy grouping", "[Print][DynaPin]") {
    GIVEN("Two copies of the same model") {
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_support", true },
            { "enable_dynapin_support_optimization", false }
        });
        init_dynapin_print(print, model, config);

        ModelObject *model_object = model.objects.front();
        ModelInstance *second_instance = model_object->add_instance(*model_object->instances.front());
        second_instance->set_offset({140.0, 100.0, 0.0});

        WHEN("DynaPin is disabled") {
            print.apply(model, config);

            THEN("Copies remain grouped in one PrintObject") {
                REQUIRE(print.objects().size() == 1);
                REQUIRE(print.objects().front()->instances().size() == 2);
            }
        }

        WHEN("DynaPin is enabled") {
            config.set("enable_dynapin_support_optimization", true);
            print.apply(model, config);

            THEN("Each copy receives its own PrintObject") {
                REQUIRE(print.objects().size() == 2);
                for (const PrintObject *object : print.objects())
                    REQUIRE(object->instances().size() == 1);
            }
        }

        WHEN("DynaPin is enabled and then disabled again") {
            config.set("enable_dynapin_support_optimization", true);
            print.apply(model, config);
            REQUIRE(print.objects().size() == 2);

            config.set("enable_dynapin_support_optimization", false);
            print.apply(model, config);

            THEN("The copies are merged back into the normal representation") {
                REQUIRE(print.objects().size() == 1);
                REQUIRE(print.objects().front()->instances().size() == 2);
            }
        }

        WHEN("A DynaPin copy is removed") {
            config.set("enable_dynapin_support_optimization", true);
            print.apply(model, config);
            REQUIRE(print.objects().size() == 2);

            model_object->delete_instance(1);
            print.apply(model, config);

            THEN("Only the remaining copy is retained") {
                REQUIRE(print.objects().size() == 1);
                REQUIRE(print.objects().front()->instances().size() == 1);
            }
        }
    }
}

SCENARIO("Print: DynaPin copies keep independent fixed-coordinate supports", "[Print][DynaPin]") {
    GIVEN("Two moved copies and a printer DynaPin configuration") {
        ResourcesDirGuard resources_guard(test_resources_dir());
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_support", true },
            { "enable_dynapin_support_optimization", true },
            { "dynapin_config_path", "Kingroon/dynapin/kp3s.json" },
            { "dynapin_selected_pins", "1,0" },
            { "printable_area", "0x0,180x0,180x180,0x180" }
        });
        init_dynapin_print(print, model, config);

        ModelObject *model_object = model.objects.front();
        ModelInstance *second_instance = model_object->add_instance(*model_object->instances.front());
        second_instance->set_offset({140.0, 100.0, 0.0});
        print.apply(model, config);
        REQUIRE(print.objects().size() == 2);

        const PrintObject *first_object  = print.objects().front();
        const PrintObject *second_object = print.objects().back();
        REQUIRE(first_object->instances().size() == 1);
        REQUIRE(second_object->instances().size() == 1);
        REQUIRE(print.dynapin_selection().source == DynaPin::SelectionSource::Manual);
        REQUIRE(print.dynapin_selection().pins == std::vector<DynaPin::Pin>{{1, 0}});

        const auto first_blockers  = DynaPin::support_blocker_regions_local(*first_object);
        const auto second_blockers = DynaPin::support_blocker_regions_local(*second_object);
        REQUIRE(first_blockers.size() == 1);
        REQUIRE(second_blockers.size() == 1);
        CHECK(first_blockers.front().z_min == Catch::Approx(9.7));
        CHECK(first_blockers.front().z_max == Catch::Approx(14.7));

        const Point first_shift = first_object->instances().front().shift_without_plate_offset();
        const BoundingBox first_blocker_box = get_extents(first_blockers.front().poly);
        CHECK(unscale<double>(first_blocker_box.min.x() + first_shift.x()) == Catch::Approx(20.0));
        CHECK(unscale<double>(first_blocker_box.max.x() + first_shift.x()) == Catch::Approx(180.0));

        const auto first_surfaces = DynaPin::pin_top_surfaces_for_object(*first_object);
        REQUIRE(first_surfaces.size() == 1);
        const BoundingBox first_surface_box = get_extents(first_surfaces.front().poly);
        CHECK(unscale<double>(first_surface_box.min.x() + first_shift.x()) == Catch::Approx(20.0));
        CHECK(unscale<double>(first_surface_box.max.x() + first_shift.x()) == Catch::Approx(180.0));

        REQUIRE(first_blockers.front().poly.points.front() != second_blockers.front().poly.points.front());

        const Point first_world_point = first_blockers.front().poly.points.front() +
                                        first_object->instances().front().shift_without_plate_offset();
        const Point second_world_point = second_blockers.front().poly.points.front() +
                                         second_object->instances().front().shift_without_plate_offset();
        REQUIRE(first_world_point == second_world_point);

        print.process();
        THEN("Support layers are generated independently") {
            REQUIRE(first_object->get_shared_object() == nullptr);
            REQUIRE(second_object->get_shared_object() == nullptr);
            const std::vector<DynaPin::BlockerBox> boxes = DynaPin::selected_blocker_boxes(print);
            REQUIRE(boxes.size() == 1);
            CHECK(boxes.front().pin == DynaPin::Pin{1, 0});
            CHECK(boxes.front().min.x() == Catch::Approx(20.0));
            CHECK(boxes.front().max.x() == Catch::Approx(180.0));
        }

        WHEN("The second model copy is moved") {
            const ObjectID moved_instance_id = second_instance->id();
            second_instance->set_offset({60.0, 0.0, 0.0});
            print.apply(model, config);

            THEN("The matching PrintObject is reused and its support is invalidated") {
                const PrintObject *moved_object = nullptr;
                for (const PrintObject *object : print.objects())
                    if (object->instances().front().model_instance->id() == moved_instance_id)
                        moved_object = object;
                REQUIRE(moved_object != nullptr);
                REQUIRE_FALSE(moved_object->is_step_done(posSupportMaterial));
            }
        }
    }
}

SCENARIO("Print: Brim generation", "[Print][.]") {
    GIVEN("20mm cube and default config, 1mm first layer width") {
        WHEN("Brim is set to 3mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "first_layer_extrusion_width", 	1 },
	        	{ "brim_width", 					3 }
	        });
            THEN("Brim Extrusion collection has 3 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 3);
            }
        }
        WHEN("Brim is set to 6mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "first_layer_extrusion_width", 	1 },
	        	{ "brim_width", 					6 }
	        });
            THEN("Brim Extrusion collection has 6 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 6);
            }
        }
        WHEN("Brim is set to 6mm, extrusion width 0.5mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "first_layer_extrusion_width", 	1 },
	        	{ "brim_width", 					6 },
	        	{ "first_layer_extrusion_width", 	0.5 }
	        });
			print.process();
            THEN("Brim Extrusion collection has 12 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 14);
            }
        }
    }
}

TEST_CASE("Tree support node growth is isolated between concurrent objects", "[Print][DynaPin]")
{
    Print              print_a, print_b;
    Model              model_a, model_b;
    DynamicPrintConfig config_a = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig config_b = config_a;
    config_a.set_deserialize_strict({{"tree_support_branch_diameter_angle", 5.}});
    config_b.set_deserialize_strict({{"tree_support_branch_diameter_angle", 25.}});
    init_dynapin_print(print_a, model_a, config_a);
    init_dynapin_print(print_b, model_b, config_b);
    TreeSupportData data_a(*print_a.objects().front(), 0., 0.1);
    TreeSupportData data_b(*print_b.objects().front(), 0., 0.1);
    auto            grow = [](TreeSupportData& data) {
        auto* parent = data.create_node(Point(0, 0), 0, 1, 0, true, nullptr, 2., 1., 1., 1.);
        return data.create_node(Point(0, 0), 1, 0, 0, true, parent, 1., 1., 2.)->radius;
    };
    auto first  = std::async(std::launch::async, [&]() { return grow(data_a); });
    auto second = std::async(std::launch::async, [&]() { return grow(data_b); });
    CHECK(first.get() == Catch::Approx(1. + 5. * M_PI / 180.));
    CHECK(second.get() == Catch::Approx(1. + 25. * M_PI / 180.));
}

TEST_CASE("Concurrent Tree angle evaluations match serial geometry across styles", "[Print][DynaPin][DynaPinPlacement]")
{
    ResourcesDirGuard  resources_guard(test_resources_dir());
    DebugStageEnvGuard env_guard;
    REQUIRE(::setenv("DYNAPIN_DEBUG_STAGE", "2", 1) == 0);
    std::vector<DynaPin::PlacementTask> tasks;
    for (const char* style : {"tree_slim", "tree_strong", "tree_hybrid"}) {
        for (double angle : {5., 25.}) {
            Print        print;
            Model        model;
            TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
            mesh.scale(Vec3f(1.f, 1.f, 0.25f));
            mesh.translate(30.f, 30.f, 0.f);
            TriangleMesh top = Test::mesh(TestMesh::cube_20x20x20);
            top.scale(Vec3f(0.1f, 0.1f, 0.1f));
            top.translate(11.5f, 11.5f, 23.f);
            mesh.merge(top);
            ModelObject* object = model.add_object();
            object->add_volume(std::move(mesh));
            ModelInstance* instance = object->add_instance();
            instance->set_offset({100., 0., 0.});
            object->ensure_on_bed();
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_deserialize_strict({
                {"enable_support", true},
                {"enable_dynapin_support_optimization", true},
                {"enable_dynapin_placement_optimization", true},
                {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
                {"dynapin_selected_pins", ""},
                {"support_type", "tree(auto)"},
                {"support_style", style},
                {"tree_support_branch_diameter_angle", angle},
                {"support_interface_bottom_layers", 1},
                {"printable_area", "0x0,180x0,180x180,0x180"},
                {"printable_height", 180.},
            });
            print.auto_assign_extruders(object);
            print.apply(model, config);
            print.set_status_silent();
            auto preparation = DynaPin::prepare_placement_task(model, print, {{0., 0.}, {180., 0.}, {180., 180.}, {0., 180.}}, 180., {}, 0);
            REQUIRE(preparation.task);
            tasks.push_back(std::move(*preparation.task));
        }
    }
    const std::vector<DynaPin::PlacementCandidate> candidates{{90., 0.}, {90., 20.}, {90., 0.}};
    auto                                           evaluate = [&](const DynaPin::PlacementTask& task) {
        std::vector<std::optional<double>> results;
        DynaPin::evaluate_scene_angle_group(task.scene, 90., candidates, results);
        return results;
    };
    std::vector<std::vector<std::optional<double>>> serial;
    for (const auto& task : tasks)
        serial.push_back(evaluate(task));
    REQUIRE(serial.front().front());
    std::vector<std::future<std::vector<std::optional<double>>>> futures;
    for (size_t i = 0; i < tasks.size(); ++i)
        futures.push_back(std::async(std::launch::async, [&, i]() { return evaluate(tasks[i]); }));
    for (size_t i = 0; i < tasks.size(); ++i) {
        INFO("style/config index=" << i);
        REQUIRE(serial[i][0]);
        auto parallel = futures[i].get();
        REQUIRE(parallel.size() == candidates.size());
        CHECK(parallel[0] == parallel[2]);
        CHECK_FALSE(parallel[1]);
        for (size_t j = 0; j < candidates.size(); ++j) {
            REQUIRE(bool(parallel[j]) == bool(serial[i][j]));
            if (parallel[j])
                CHECK(*parallel[j] == Catch::Approx(*serial[i][j]).margin(std::max(1e-3, std::abs(*serial[i][j]) * 1e-6)));
        }
    }
}

TEST_CASE("DynaPin Tree bounded search preserves final pins across concurrency limits", "[Print][DynaPin][DynaPinPlacement]")
{
    ResourcesDirGuard  resources_guard(test_resources_dir());
    DebugStageEnvGuard env_guard;
    TemporaryDirGuard  temp_dir_guard;
    REQUIRE(::setenv("DYNAPIN_DEBUG_STAGE", "2", 1) == 0);
    Model                     model;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext substitution{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs             plate_data;
    std::vector<Preset*>      project_presets;
    ImportedProjectDataGuard  imported_project_data_guard(plate_data, project_presets);
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const auto                fixture = (boost::filesystem::path(TEST_DATA_DIR) / "dynapin" / "Simple_Bridge.3mf").string();
    REQUIRE(load_bbs_3mf(fixture.c_str(), &config, &substitution, &model, &plate_data, &project_presets, &is_bbl_3mf, &file_version,
                         nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    config.set_deserialize_strict({
        {"enable_dynapin_support_optimization", true},
        {"enable_dynapin_placement_optimization", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_debug_stage", 2},
        {"dynapin_selected_pins", ""},
        {"support_type", "tree(auto)"},
        {"printable_area", "0x0,180x0,180x180,0x180"},
        {"printable_height", 180.},
    });
    Print initial_print;
    initial_print.set_plate_origin(Vec3d::Zero());
    for (ModelObject* object : model.objects)
        initial_print.auto_assign_extruders(object);
    initial_print.apply(model, config);
    initial_print.set_status_silent();
    auto preparation = DynaPin::prepare_placement_task(model, initial_print, {{0., 0.}, {180., 0.}, {180., 180.}, {0., 180.}}, 180., {}, 0);
    REQUIRE(preparation.task);
    auto       task          = *preparation.task;
    const auto allowed_angle = [](double angle) {
        const double normalized = DynaPin::normalized_rotation_deg(angle);
        return normalized == 0. || normalized == 5. || normalized == 355.;
    };
    task.search.delta_y_range_for_rotation = [allowed_angle](double angle) -> std::optional<DynaPin::DeltaYInterval> {
        return allowed_angle(angle) ? std::optional<DynaPin::DeltaYInterval>{{0., 0.}} : std::nullopt;
    };
    const auto scene_evaluator  = DynaPin::make_scene_angle_evaluator(task.scene);
    task.search.angle_evaluator = [allowed_angle, scene_evaluator](double angle, const std::vector<DynaPin::PlacementCandidate>& candidates,
                                                                   std::vector<std::optional<double>>&        results,
                                                                   const DynaPin::CandidateCompletedCallback& completed) {
        if (allowed_angle(angle))
            scene_evaluator(angle, candidates, results, completed);
        else
            results.assign(candidates.size(), std::nullopt);
    };
    // Exercise automatic scheduling without depending on other applications'
    // memory use. Actual memory admission/failure is tested by scheduler tests.
    task.search.resource_probe = []() -> std::optional<SystemResourceSample> {
        return SystemResourceSample{16ull << 30, 12ull << 30, 1ull << 30, 0.};
    };
    std::optional<DynaPin::PlacementResult> serial_result;
    std::vector<DynaPin::Pin>               serial_pins;
    double                                  serial_volume = 0.;
    for (size_t concurrency : {size_t(1), size_t(2), size_t(0)}) {
        INFO("angle concurrency=" << concurrency);
        task.search.angle_group_concurrency = concurrency;
        const auto result                   = DynaPin::run_placement_task(task);
        REQUIRE(result.status == DynaPin::PlacementStatus::Improved);
        Model placed_model(model);
        REQUIRE(DynaPin::apply_placement_result(placed_model, task, result).applied);
        Print placed_print;
        placed_print.set_plate_index(task.scene.plate_index);
        placed_print.set_plate_origin(task.scene.plate_origin);
        for (ModelObject* object : placed_model.objects)
            placed_print.auto_assign_extruders(object);
        placed_print.apply(placed_model, config);
        placed_print.set_status_silent();
        placed_print.process();
        const auto& pins = placed_print.dynapin_selection().pins;
        REQUIRE_FALSE(pins.empty());
        REQUIRE(placed_print.objects().front()->dynapin_tree_landing_plan().used_pins == pins);
        REQUIRE_FALSE(placed_print.objects().front()->support_layers().empty());
        double volume = 0.;
        for (const auto* layer : placed_print.objects().front()->support_layers())
            volume += layer->support_fills.total_volume();
        REQUIRE(volume > 0.);
        if (concurrency == 1) {
            serial_result = result;
            serial_pins   = pins;
            serial_volume = volume;
        } else {
            CHECK(result.candidate == serial_result->candidate);
            CHECK(result.evaluations == serial_result->evaluations);
            CHECK(result.volume_mm3 == Catch::Approx(serial_result->volume_mm3).margin(1e-3));
            CHECK(pins == serial_pins);
            CHECK(volume == Catch::Approx(serial_volume).margin(1e-3));
        }
    }
}

TEST_CASE("Tree pin selection matches the frozen selector", "[Print][DynaPin][TreePinSelection]")
{
    struct LogGuard
    {
        unsigned previous = get_logging_level();
        LogGuard() { set_logging_level(3); }
        ~LogGuard() { set_logging_level(previous); }
    } log_guard;
    ResourcesDirGuard  resources_guard(test_resources_dir());
    DebugStageEnvGuard env_guard;
    TemporaryDirGuard  temp_guard;
    REQUIRE(::setenv("DYNAPIN_DEBUG_STAGE", "2", 1) == 0);
    for (const char* fixture : {"Simple_Bridge", "RemoveGymnastBase", "Arch_Bridge"}) {
        INFO(fixture);
        Model                     model;
        DynamicPrintConfig        config;
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
        PlateDataPtrs             plates;
        std::vector<Preset*>      presets;
        ImportedProjectDataGuard  import_guard(plates, presets);
        bool                      bbl = false;
        Semver                    version;
        auto path = (boost::filesystem::path(TEST_DATA_DIR) / "dynapin" / (std::string(fixture) + ".3mf")).string();
        REQUIRE(load_bbs_3mf(path.c_str(), &config, &substitutions, &model, &plates, &presets, &bbl, &version, nullptr,
                             LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
        config.set_deserialize_strict({{"support_type", "tree(auto)"},
                                       {"support_style", "tree_slim"},
                                       {"enable_support", true},
                                       {"enable_dynapin_support_optimization", true},
                                       {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"}});
        Print print;
        print.set_plate_origin(Vec3d::Zero());
        for (auto* object : model.objects)
            print.auto_assign_extruders(object);
        print.apply(model, config);
        print.set_status_silent();
        REQUIRE(print.prepare_slices_for_support_geometry());
        DynaPin::Config pins_config;
        REQUIRE(DynaPin::load_config_for_print(print, pins_config));
        const auto candidates = DynaPin::candidate_pins(pins_config);
        REQUIRE_FALSE(candidates.empty());
        for (const auto& pin : candidates)
            CHECK(DynaPin::pin_collides_with_model(print, pins_config, pin) ==
                  Test::reference_pin_collides_with_model(print, pins_config, pin));
        for (auto* object : print.objects()) {
            const auto* layer_cache   = &object->dynapin_layer_polygons();
            const auto* contact_cache = &object->dynapin_tree_contacts();
            for (int variant = 0; variant < 3; ++variant) {
                INFO("variant=" << variant);
                CHECK(&object->dynapin_layer_polygons() == layer_cache);
                CHECK(&object->dynapin_tree_contacts() == contact_cache);
                std::vector<DynaPin::Pin> colliding;
                for (size_t i = 0; i < candidates.size(); ++i)
                    if (variant == 2 || (variant == 1 && i % 3 == 0))
                        colliding.push_back(candidates[i]);
                auto         start        = std::chrono::steady_clock::now();
                const auto   expected     = Test::reference_select_tree_pins(*object, pins_config, candidates, colliding);
                const double reference_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                start                     = std::chrono::steady_clock::now();
                const auto   actual       = DynaPin::select_tree_pins(*object, pins_config, candidates, colliding);
                const double optimized_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                CHECK(actual.selected == expected.selected);
                CHECK(actual.rejected_collisions == expected.rejected_collisions);
                std::cout << "PIN_SELECTION fixture=" << fixture << " variant=" << variant << " reference_ms=" << reference_ms
                          << " optimized_ms=" << optimized_ms << " selected=" << actual.selected.size() << std::endl;
                for (auto& instance : object->instances())
                    instance.shift += Point::new_scale(0., 0.125);
            }
        }
    }
}

TEST_CASE("Tree pin geometry cache follows slicing and cancellation", "[Print][DynaPin][TreePinSelection]")
{
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    Model model;
    auto  config = DynamicPrintConfig::full_print_config();
    init_dynapin_print(print, model, config);
    REQUIRE(print.prepare_slices_for_support_geometry());
    const auto* object = print.objects().front();
    print.cancel();
    CHECK_THROWS(object->dynapin_layer_polygons());
    print.restart();
    const auto* cache     = &object->dynapin_layer_polygons();
    const auto  old_count = cache->size();
    print.cancel();
    CHECK_THROWS(object->dynapin_tree_contacts());
    print.restart();
    CHECK(object->dynapin_tree_contacts().empty());
    config.set_deserialize_strict({{"support_top_z_distance", 0.35}});
    print.apply(model, config);
    REQUIRE(print.objects().front() == object);
    // Support distance also invalidates slicing in the existing pipeline.
    CHECK(object->dynapin_layer_polygons().size() == old_count);
    config.set_deserialize_strict({{"layer_height", 0.25}});
    print.apply(model, config);
    REQUIRE(print.prepare_slices_for_support_geometry());
    object              = print.objects().front();
    const auto& rebuilt = object->dynapin_layer_polygons();
    CHECK(rebuilt.size() != old_count);
    REQUIRE(rebuilt.size() == object->layers().size());
    for (size_t i = 0; i < rebuilt.size(); ++i)
        CHECK(rebuilt[i] == to_polygons(object->layers()[i]->lslices));
    CHECK(object->dynapin_tree_contacts().empty());
    const size_t rebuilt_count   = rebuilt.size();
    const auto   before_rotation = rebuilt;
    model.objects.front()->instances.front()->set_rotation(Vec3d(0., 0., 0.1));
    print.apply(model, config);
    REQUIRE(print.prepare_slices_for_support_geometry());
    CHECK(print.objects().front()->dynapin_layer_polygons() != before_rotation);
    const auto before_shape_change = print.objects().front()->dynapin_layer_polygons();
    model.objects.front()->scale(1.1, 1., 1.);
    print.apply(model, config);
    REQUIRE(print.prepare_slices_for_support_geometry());
    CHECK(print.objects().front()->dynapin_layer_polygons() != before_shape_change);
    print.clear();
    print.apply(model, config);
    REQUIRE(print.prepare_slices_for_support_geometry());
    CHECK(print.objects().front()->dynapin_layer_polygons().size() == rebuilt_count);
    print.get_object(0)->clear_layers();
    CHECK(print.objects().front()->dynapin_layer_polygons().empty());
    CHECK(print.objects().front()->dynapin_tree_contacts().empty());
    Print shared_print;
    shared_print.set_plate_origin(Vec3d::Zero());
    Model shared_model;
    init_dynapin_print(shared_print, shared_model, config);
    REQUIRE(shared_print.prepare_slices_for_support_geometry());
    auto* shared_source = shared_print.get_object(0);
    auto* destination   = print.get_object(0);
    destination->set_shared_object(shared_source);
    destination->copy_layers_from_shared_object();
    CHECK(destination->dynapin_layer_polygons() == shared_source->dynapin_layer_polygons());
    destination->clear_shared_object();
    CHECK(destination->dynapin_layer_polygons().empty());
    CHECK(destination->dynapin_tree_contacts().empty());
}

TEST_CASE("Tree pin projection preserves boundary and height ordering", "[Print][DynaPin][TreePinSelection]")
{
    const Polygon contact{{0, 0}, {10000, 0}, {10000, 10000}, {0, 10000}};
    for (coord_t x : {coord_t(-1), coord_t(0), coord_t(9999), coord_t(10000), coord_t(10001)}) {
        Polygon surface{{x, 0}, {x + 10000, 0}, {x + 10000, 10000}, {x, 10000}};
        for (bool colliding : {false, true}) {
            for (double z : {10., 20., 30.}) {
                std::vector<DynaPin::ProjectionEvent> events{
                    {DynaPin::ProjectionEventType::Contact, 30., {contact}, {}, false},
                    {DynaPin::ProjectionEventType::PinSurface, z, {surface}, {1, 0}, colliding},
                    {DynaPin::ProjectionEventType::PinSurface, z, {contact}, {1, 1}, false},
                    {DynaPin::ProjectionEventType::Model, 15., {surface}, {}, false},
                    {DynaPin::ProjectionEventType::Contact, 5., {contact}, {}, false},
                    {DynaPin::ProjectionEventType::PinSurface, 2., {contact}, {0, 0}, false},
                };
                const auto expected = Test::reference_select_from_projection(events);
                const auto actual   = DynaPin::select_from_projection(events);
                CHECK(actual.selected == expected.selected);
                CHECK(actual.rejected_collisions == expected.rejected_collisions);
            }
        }
    }
    CHECK(DynaPin::select_from_projection({}).selected.empty());
}

TEST_CASE("Tree pin projection batches contacts without changing selections", "[Print][DynaPin][TreePinSelection]")
{
    std::mt19937                       rng(7241);
    std::uniform_int_distribution<int> coordinate(-20, 20);
    for (int trial = 0; trial < 128; ++trial) {
        CAPTURE(trial);
        std::vector<DynaPin::ProjectionEvent> events;
        for (int i = 0; i < 40; ++i) {
            const coord_t x = coordinate(rng) * 10000;
            const coord_t y = coordinate(rng) * 10000;
            Polygon       outer{{x, y}, {x + 100000, y}, {x + 100000, y + 100000}, {x, y + 100000}};
            Polygon       hole{{x + 20000, y + 20000}, {x + 80000, y + 20000}, {x + 80000, y + 80000}, {x + 20000, y + 80000}};
            outer.rotate((trial % 9) * 0.1);
            hole.rotate((trial % 9) * 0.1);
            events.push_back({DynaPin::ProjectionEventType::Contact, double(i), diff({outer}, {hole}), {}, false});
        }
        for (int i = 0; i < 12; ++i) {
            const coord_t x = coordinate(rng) * 10000 + (i % 3 - 1);
            const coord_t y = coordinate(rng) * 10000;
            Polygon       surface{{x, y}, {x + 50000, y}, {x + 50000, y + 50000}, {x, y + 50000}};
            events.push_back({DynaPin::ProjectionEventType::PinSurface, double(i * 3), {surface}, {i, 0}, i % 3 == 0});
            if (i % 2 == 0)
                events.push_back({DynaPin::ProjectionEventType::Model, double(i * 3 + 1), {surface}, {}, false});
        }
        std::shuffle(events.begin(), events.end(), rng);
        const auto expected = Test::reference_select_from_projection(events);
        const auto actual   = DynaPin::select_from_projection(events);
        CHECK(actual.selected == expected.selected);
        CHECK(actual.rejected_collisions == expected.rejected_collisions);
    }
}

TEST_CASE("Tree pin selection preserves multi-stage overhang choices", "[Print][DynaPin][TreePinSelection]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    for (const char* style : {"tree_slim", "tree_strong", "tree_hybrid"}) {
        for (double angle : {5., 25.}) {
            Model        model;
            TriangleMesh mesh = make_cube(5., 5., 10.);
            auto         deck = make_cube(70., 15., 3.);
            deck.translate(0., 0., 10.);
            mesh.merge(deck);
            auto stem = make_cube(5., 5., 10.);
            stem.translate(0., 0., 13.);
            mesh.merge(stem);
            auto upper = make_cube(70., 15., 3.);
            upper.translate(0., 10., 23.);
            mesh.merge(upper);
            auto* object = model.add_object();
            object->add_volume(std::move(mesh));
            object->add_instance()->set_offset({90., 70., 0.});
            object->ensure_on_bed();
            auto config = DynamicPrintConfig::full_print_config();
            config.set_deserialize_strict({{"enable_support", true},
                                           {"support_type", "tree(auto)"},
                                           {"support_style", style},
                                           {"tree_support_branch_diameter_angle", angle},
                                           {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
                                           {"printable_area", "0x0,180x0,180x180,0x180"}});
            Print print;
            print.set_plate_origin(Vec3d::Zero());
            print.auto_assign_extruders(object);
            print.apply(model, config);
            print.set_status_silent();
            REQUIRE(print.prepare_slices_for_support_geometry());
            DynaPin::Config pins_config;
            REQUIRE(DynaPin::load_config_for_print(print, pins_config));
            const auto candidates = DynaPin::candidate_pins(pins_config);
            for (double branch_angle : {0., 45., 89.}) {
                config.set_deserialize_strict({{"tree_support_branch_angle", branch_angle}});
                print.apply(model, config);
                REQUIRE(print.prepare_slices_for_support_geometry());
                const auto*                     printed = print.objects().front();
                const std::vector<DynaPin::Pin> colliding{{0, 0}, {1, 1}, {2, 2}};
                REQUIRE(printed->dynapin_tree_contacts().size() >= 2);
                for (double row_pitch : {pins_config.row_pitch_z, 0., EPSILON / 2.}) {
                    auto heights        = pins_config;
                    heights.row_pitch_z = row_pitch;
                    const auto expected = Test::reference_select_tree_pins(*printed, heights, candidates, colliding);
                    const auto actual   = DynaPin::select_tree_pins(*printed, heights, candidates, colliding);
                    CHECK(actual.selected == expected.selected);
                    CHECK(actual.rejected_collisions == expected.rejected_collisions);
                }
            }
        }
    }
}

TEST_CASE("DynaPin stabilization includes the full pin width beyond the landing inset", "[DynaPin][SupportSpeed]")
{
    ResourcesDirGuard  resources_guard(test_resources_dir());
    TemporaryDirGuard  temporary_guard;
    DebugStageEnvGuard stage_guard;
    PullConfigFile     file;
    auto               json                           = pull_reference_fixtures()[0]["config"];
    json["support_exclusion"]["blocker_width_y"]      = 12.4;
    json["support_exclusion"]["landing_edge_inset_y"] = 1.2;
    file.write(json);
    Print print;
    Model model;
    auto  config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"enable_dynapin_support_optimization", true},
                                   {"dynapin_debug_stage", 2},
                                   {"dynapin_config_path", file.path.string()},
                                   {"dynapin_selected_pins", "0,0"}});
    for (const auto& key : config.keys()) {
        auto* option = config.option(key);
        if (option->type() == coEnums) {
            const auto* definition = print_config_def.get(key);
            if (option->nullable())
                static_cast<ConfigOptionEnumsGenericNullable*>(option)->keys_map = definition->enum_keys_map;
            else
                static_cast<ConfigOptionEnumsGeneric*>(option)->keys_map = definition->enum_keys_map;
        }
    }
    init_dynapin_print(print, model, config);
    print.process();
    auto* object = print.get_object(size_t(0));
    REQUIRE(object->support_layers().empty());
    const auto surfaces = DynaPin::landing_surfaces_for_object(*object);
    REQUIRE_FALSE(surfaces.empty());
    const auto&  surface   = surfaces.front();
    auto         bounds    = get_extents(surface.poly);
    const double x         = unscale<double>(bounds.center().x());
    const double y         = unscale<double>(bounds.center().y());
    const auto   rectangle = [&](double half_width) {
        return Polygon(Points{Point::new_scale(x - 2., y - half_width), Point::new_scale(x + 2., y - half_width),
                              Point::new_scale(x + 2., y + half_width), Point::new_scale(x - 2., y + half_width)});
    };
    for (int i = 0; i < 5; ++i) {
        auto* layer = object->add_support_layer(i, i, 0.2, surface.print_z + 0.2 * (i + 1));
        if (i == 0)
            layer->dynapin_landing_regions = {rectangle(5.)};
        for (double dy : {0., -5.8, 5.8, -6.5, 6.5}) {
            ExtrusionPath path(erSupportMaterial, 0.1, 0.4f, 0.2f);
            path.polyline           = Polyline(Points{Point::new_scale(x - 2., y + dy), Point::new_scale(x + 2., y + dy)});
            path.is_dynapin_landing = i == 0;
            layer->support_fills.append(path);
        }
    }
    const auto regions = DynaPin::support_stabilization_regions(*object, 3);
    REQUIRE(regions.size() == 5);
    CHECK(regions[0].empty());
    CHECK(regions[4].empty());
    for (size_t i = 1; i <= 3; ++i) {
        for (double dy : {-5.8, 5.8})
            CHECK_FALSE(
                intersection_ln(Lines{Line(Point::new_scale(x - 1., y + dy), Point::new_scale(x + 1., y + dy))}, regions[i]).empty());
        for (double dy : {-6.5, 6.5})
            CHECK(intersection_ln(Lines{Line(Point::new_scale(x - 1., y + dy), Point::new_scale(x + 1., y + dy))}, regions[i]).empty());
    }
}

TEST_CASE("DynaPin final G-code slows X support extrusion for exactly three layers", "[DynaPin][SupportSpeed]")
{
    ResourcesDirGuard   resources_guard(test_resources_dir());
    TemporaryDirGuard   temporary_guard;
    DebugStageEnvGuard  stage_guard;
    const bool          arc_fitting   = GENERATE(false, true);
    const bool          mixed_heights = GENERATE(false, true);
    const ExtrusionRole support_role  = GENERATE(erSupportMaterial, erSupportMaterialInterface, erSupportTransition);
    const std::string   role_label    = ExtrusionEntity::role_to_string(support_role);
    auto                json          = pull_reference_fixtures()[0]["config"];
    json["support_speed"]             = {{"layers", 3}, {"max_x_speed", 0.}};
    PullConfigFile file;
    file.write(json);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"enable_dynapin_support_optimization", true},
                                   {"dynapin_debug_stage", 2},
                                   {"dynapin_config_path", file.path.string()},
                                   {"dynapin_selected_pins", "0,0"},
                                   {"enable_support", true},
                                   {"support_type", "normal(auto)"},
                                   {"initial_layer_speed", 20.},
                                   {"support_speed", 80.},
                                   {"support_interface_speed", "100%"},
                                   {"bridge_speed", 80.},
                                   {"slow_down_layers", 0},
                                   {"max_volumetric_extrusion_rate_slope", 20.},
                                   {"fan_speedup_time", 1.},
                                   {"slow_down_for_layer_cooling", "0"},
                                   {"filament_max_volumetric_speed", "1000"},
                                   {"enable_arc_fitting", arc_fitting}});
    Print print;
    Model model;
    for (const std::string& key : config.keys()) {
        ConfigOption* option = config.option(key);
        if (option->type() == coEnums) {
            const auto* definition = print_config_def.get(key);
            if (option->nullable())
                static_cast<ConfigOptionEnumsGenericNullable*>(option)->keys_map = definition->enum_keys_map;
            else
                static_cast<ConfigOptionEnumsGeneric*>(option)->keys_map = definition->enum_keys_map;
        }
    }
    init_dynapin_print(print, model, config);
    print.process();
    PrintObject* object = print.get_object(size_t(0));
    REQUIRE(object->support_layers().empty());
    const Polygon footprint(
        Points{Point::new_scale(-9., -9.), Point::new_scale(9., -9.), Point::new_scale(9., 9.), Point::new_scale(-9., 9.)});
    for (int i = 0; i < 5; ++i) {
        SupportLayer* layer = object->add_support_layer(i, i, mixed_heights ? 0.1 : 0.2, 7.5 + i * 0.2);
        if (!mixed_heights || i > 0)
            layer->support_islands = {ExPolygon(footprint)};
        if (i == 0)
            layer->dynapin_landing_regions = {footprint};
        ExtrusionPath path(support_role, 0.1, 0.5f, 0.2f);
        path.polyline           = Polyline(Points{Point::new_scale(-8., -8.), Point::new_scale(-6., -8.), Point::new_scale(-6., -6.),
                                                  Point::new_scale(8., -6.), Point::new_scale(8., 8.), Point::new_scale(-8., -8.)});
        path.is_dynapin_landing = i == 0;
        if (arc_fitting) {
            path.simplify_by_fitting_arc(scale_(0.01));
            REQUIRE_FALSE(path.polyline.fitting_result.empty());
        }
        layer->support_fills.append(path);
    }
    const auto output_path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("dynapin-speed-%%%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(output_path.string(), &result, nullptr);
    std::ifstream     stream(output_path.string());
    const std::string output((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    boost::filesystem::remove(output_path);
    const std::string role_prefix = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
    REQUIRE_FALSE(output.empty());
    REQUIRE(output.find(role_prefix + role_label) != std::string::npos);
    REQUIRE(output.find('\0') == std::string::npos);
    size_t        callbacks = 0, support_tags = 0, support_moves = 0;
    bool          support = false;
    std::set<int> capped_layers;
    size_t        normal_y = 0, normal_later = 0, diagonal = 0, short_x = 0;
    GCodeReader   reader;
    reader.apply_config(print.config());
    reader.parse_buffer(output, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
        ++callbacks;
        if (line.raw().find(role_prefix) == 0) {
            support = line.raw() == role_prefix + role_label;
            support_tags += support;
        }
        support_moves += support && line.has_x();
        if (!support || !line.extruding(state) || !line.cmd_is("G1") || (!line.has_x() && !line.has_y()))
            return;
        const int    index = int(std::round((state.z() - 7.5) / 0.2));
        const double dx    = std::abs(line.new_X(state) - state.x());
        const double dy    = std::abs(line.new_Y(state) - state.y());
        const double speed = line.new_F(state) / 60.;
        if (index >= 1 && index <= 3) {
            if (dx > 0.001) {
                CHECK(speed * dx / std::hypot(dx, dy) <= 20.01);
                capped_layers.insert(index);
                diagonal += dy > 5.;
                short_x += dx < 5.;
            } else if (dy > 5.) {
                CHECK(speed == Catch::Approx(80.).margin(0.01));
                ++normal_y;
            }
        } else if (index == 4) {
            CHECK(speed == Catch::Approx(80.).margin(0.01));
            ++normal_later;
        }
    });
    CAPTURE(callbacks, support_tags, support_moves);
    CHECK(capped_layers == std::set<int>{1, 2, 3});
    CHECK(diagonal >= 3);
    CHECK(short_x >= 3);
    CHECK(normal_y >= 3);
    CHECK(normal_later >= 3);
}

TEST_CASE("DynaPin caps travel approaching and leaving stabilized support", "[DynaPin][SupportSpeed]")
{
    ResourcesDirGuard   resources_guard(test_resources_dir());
    TemporaryDirGuard   temporary_guard;
    DebugStageEnvGuard  stage_guard;
    const bool          arc_fitting    = GENERATE(false, true);
    const ExtrusionRole support_role   = GENERATE(erSupportMaterial, erSupportMaterialInterface, erSupportTransition);
    const std::string   hop_type       = GENERATE("Normal Lift", "Slope Lift", "Spiral Lift");
    const int           mode           = GENERATE(0, 1, 2); // Disabled, zero protected layers, enabled.
    const int           limited_layers = mode == 2 ? 3 : 0;
    const std::string   role_label     = ExtrusionEntity::role_to_string(support_role);
    auto                json           = pull_reference_fixtures()[0]["config"];
    json["support_speed"]              = {{"layers", mode == 1 ? 0 : 3}, {"max_x_speed", 0.}};
    PullConfigFile file;
    file.write(json);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    for (const std::string& key : config.keys()) {
        ConfigOption* option = config.option(key);
        if (option->type() == coEnums) {
            const auto* definition = print_config_def.get(key);
            if (option->nullable())
                static_cast<ConfigOptionEnumsGenericNullable*>(option)->keys_map = definition->enum_keys_map;
            else
                static_cast<ConfigOptionEnumsGeneric*>(option)->keys_map = definition->enum_keys_map;
        }
    }
    config.set_deserialize_strict({{"enable_dynapin_support_optimization", mode != 0},
                                   {"dynapin_debug_stage", 2},
                                   {"dynapin_config_path", file.path.string()},
                                   {"dynapin_selected_pins", "0,0"},
                                   {"enable_support", true},
                                   {"support_type", "normal(auto)"},
                                   {"initial_layer_speed", 20.},
                                   {"support_speed", 80.},
                                   {"travel_speed", 150.},
                                   {"travel_speed_z", 100.},
                                   {"z_hop", "0.4"},
                                   {"z_hop_types", hop_type},
                                   {"retraction_minimum_travel", "0"},
                                   {"wipe", "0"},
                                   {"support_interface_speed", "100%"},
                                   {"bridge_speed", 80.},
                                   {"slow_down_layers", 0},
                                   {"max_volumetric_extrusion_rate_slope", 20.},
                                   {"fan_speedup_time", 1.},
                                   {"slow_down_for_layer_cooling", "0"},
                                   {"filament_max_volumetric_speed", "1000"},
                                   {"enable_arc_fitting", arc_fitting}});
    Print print;
    Model model;
    init_dynapin_print(print, model, config);
    print.process();
    PrintObject* object = print.get_object(size_t(0));
    REQUIRE(object->support_layers().empty());
    const Polygon footprint(
        Points{Point::new_scale(-9., -9.), Point::new_scale(9., -9.), Point::new_scale(9., 9.), Point::new_scale(-9., 9.)});
    for (int i = 0; i < 5; ++i) {
        SupportLayer* layer    = object->add_support_layer(i, i, 0.2, 7.5 + i * 0.2);
        layer->support_islands = {ExPolygon(footprint)};
        if (i == 0)
            layer->dynapin_landing_regions = {footprint};
        ExtrusionPath path(support_role, 0.1, 0.5f, 0.2f);
        path.polyline           = Polyline(Points{Point::new_scale(-8., -8.), Point::new_scale(-6., -8.), Point::new_scale(-6., -6.),
                                                  Point::new_scale(8., -6.), Point::new_scale(8., 8.), Point::new_scale(-8., -8.)});
        path.is_dynapin_landing = i == 0;
        if (arc_fitting) {
            path.simplify_by_fitting_arc(scale_(0.01));
            REQUIRE_FALSE(path.polyline.fitting_result.empty());
        }
        ExtrusionEntityCollection ordered;
        ordered.no_sort = true;
        ordered.append(path);
        for (double x : {20., 30., 6.}) {
            path.polyline = Polyline(Points{Point::new_scale(x, 5.), Point::new_scale(x + 2., 5.)});
            ordered.append(path);
        }
        layer->support_fills.append(ordered);
    }
    const auto output_path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("dynapin-speed-%%%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(output_path.string(), &result, nullptr);
    std::ifstream     stream(output_path.string());
    const std::string output((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (mode == 0) {
        // A configured limit must be inert when DynaPin itself is disabled.
        json["support_speed"]["layers"] = 0;
        file.write(json);
        print.export_gcode(output_path.string(), &result, nullptr);
        std::ifstream     reference_stream(output_path.string());
        const std::string reference((std::istreambuf_iterator<char>(reference_stream)), std::istreambuf_iterator<char>());
        auto              without_timestamp = [](const std::string& text) {
            std::istringstream lines(text);
            std::string        normalized, line;
            while (std::getline(lines, line))
                if (line.rfind("; generated by ", 0) != 0)
                    normalized += line + '\n';
            return normalized;
        };
        CHECK((without_timestamp(output) == without_timestamp(reference)));
    }
    boost::filesystem::remove(output_path);
    const std::string role_prefix        = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
    bool              support            = false;
    int               previous_index     = -1;
    bool              previous_protected = false;
    const Point       shift              = object->instances().front().shift;
    struct TravelMove
    {
        double x_speed;
        double speed;
        std::string raw;
    };
    std::vector<TravelMove> pending;
    std::set<int>           approaches, departures, ordinary, outside;
    GCodeReader             reader;
    reader.apply_config(print.config());
    reader.parse_buffer(output, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
        if (line.raw().find(role_prefix) == 0)
            support = line.raw() == role_prefix + role_label;
        if (!line.cmd_is("G1") && !line.cmd_is("G2") && !line.cmd_is("G3"))
            return;
        const double dx    = std::abs(line.new_X(state) - state.x());
        const double dy    = std::abs(line.new_Y(state) - state.y());
        const double dz    = std::abs(line.new_Z(state) - state.z());
        const double speed = line.new_F(state) / 60.;
        if (line.extruding(state) && (line.has_x() || line.has_y())) {
            const int  index           = support ? int(std::round((state.z() - 7.5) / 0.2)) : -1;
            const bool protected_layer = limited_layers > 0 && index >= 1 && index <= 3;
            const bool approaching     = protected_layer && footprint.contains(Point::new_scale(state.x(), state.y()) - shift);
            const bool departing       = previous_protected;
            for (const auto& move : pending) {
                if (approaching || departing) {
                    CAPTURE(hop_type, arc_fitting, index, previous_index, move.raw);
                    CHECK(move.x_speed <= 20.02);
                    if (approaching)
                        approaches.insert(index);
                    if (departing)
                        departures.insert(previous_index);
                } else if (index >= 1 && index == previous_index) {
                    CHECK(move.speed >= 99.99); // Spirals use travel_speed_z (100 mm/s).
                    if (move.speed > 149.99) {
                        if (index == 4)
                            ordinary.insert(index);
                        else
                            outside.insert(index);
                    }
                }
            }
            pending.clear();
            previous_index     = index;
            previous_protected = protected_layer && footprint.contains(Point::new_scale(line.new_X(state), line.new_Y(state)) - shift);
        } else if (dx > 0.01 || dy > 0.01 || line.cmd_is("G2") || line.cmd_is("G3")) {
            // A spiral has X tangents even when its endpoints share the same XY.
            const double x_speed = line.cmd_is("G1") ? speed * dx / std::sqrt(dx * dx + dy * dy + dz * dz) : speed;
            pending.push_back({x_speed, speed, line.raw()});
        }
    });
    if (limited_layers > 0) {
        CHECK(approaches == std::set<int>{1, 2, 3});
        CHECK(departures == std::set<int>{1, 2, 3});
    } else {
        CHECK(approaches.empty());
        CHECK(departures.empty());
    }
    CHECK(ordinary == std::set<int>{4});
    CHECK(outside == std::set<int>{1, 2, 3});
}

TEST_CASE("DynaPin pulls share one lift and restore the saved position", "[DynaPin][DynaPinPull]")
{
    const std::string  mode = GENERATE("normal", "reverted", "alternating");
    TemporaryDirGuard  temporary_guard;
    DebugStageEnvGuard stage_guard;
    auto               json    = pull_reference_fixtures()[0]["config"];
    json["pull_gcode"]["type"] = mode;
    PullConfigFile file;
    file.write(json);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"enable_dynapin_support_optimization", true},
                                   {"dynapin_debug_stage", 2},
                                   {"dynapin_config_path", file.path.string()},
                                   {"dynapin_selected_pins", "0,0;0,1"},
                                   {"support_type", "normal(auto)"},
                                   {"z_hop", "0.4"},
                                   {"retract_when_changing_layer", "1"}});
    Print print;
    Model model;
    // Preset deserialization supplies these maps; static defaults only contain enum values.
    for (const std::string& key : config.keys()) {
        ConfigOption* option = config.option(key);
        if (option->type() == coEnums) {
            const auto* definition = print_config_def.get(key);
            REQUIRE(definition->enum_keys_map != nullptr);
            if (option->nullable())
                static_cast<ConfigOptionEnumsGenericNullable*>(option)->keys_map = definition->enum_keys_map;
            else
                static_cast<ConfigOptionEnumsGeneric*>(option)->keys_map = definition->enum_keys_map;
        }
    }
    init_dynapin_print(print, model, config);
    const auto export_for_test = [](Print& target) {
        const auto path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("dynapin-pull-%%%%-%%%%.gcode");
        target.process();
        GCodeProcessorResult processor_result;
        target.export_gcode(path.string(), &processor_result, nullptr);
        std::ifstream     input(path.string());
        const std::string result((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        boost::filesystem::remove(path);
        return result;
    };
    const std::string gcode                  = export_for_test(print);
    size_t            pulls                  = 0;
    size_t            returns                = 0;
    bool              in_group               = false;
    bool              previous_move_was_lift = false;
    Vec3d             saved                  = Vec3d::Zero();
    GCodeReader       reader;
    reader.parse_buffer(gcode, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
        if (line.raw().find("; BEGIN_DYNAPIN_PULL") == 0) {
            if (!in_group) {
                CHECK(previous_move_was_lift);
                saved    = Vec3d(state.x(), state.y(), state.z());
                in_group = true;
            }
            ++pulls;
        }
        if (line.raw().find("; END_DYNAPIN_RETURN") == 0) {
            REQUIRE(in_group);
            CHECK(state.x() == Catch::Approx(saved.x()));
            CHECK(state.y() == Catch::Approx(saved.y()));
            CHECK(state.z() == Catch::Approx(saved.z()));
            in_group = false;
            ++returns;
        }
        if (line.cmd_is("G1") || line.cmd_is("G0"))
            previous_move_was_lift = line.has_z() && !line.has_x() && !line.has_y() && line.z() > state.z();
    });
    CHECK(pulls == 2);
    CHECK(returns == 1);
    CHECK_FALSE(in_group);

    config.set_key_value("enable_dynapin_support_optimization", new ConfigOptionBool(false));
    Print disabled_print;
    Model disabled_model;
    init_dynapin_print(disabled_print, disabled_model, config);
    CHECK(export_for_test(disabled_print).find("BEGIN_DYNAPIN_PULL") == std::string::npos);
}

TEST_CASE("DynaPin shared front bounds support independently of pull offsets", "[DynaPin][DynaPinPull]")
{
    PullConfigFile file;
    file.write(pull_reference_fixtures()[1]["config"]);
    DynamicPrintConfig runtime = DynamicPrintConfig::full_print_config();
    runtime.set_deserialize_strict({{"dynapin_config_path", file.path.string()}, {"printable_area", "0x0,180x0,180x180,0x180"}});
    Print print;
    Model model;
    init_dynapin_print(print, model, runtime);
    DynaPin::Config config;
    REQUIRE(DynaPin::load_config_for_print(print, config));
    const PrintObject& object = *print.objects().front();
    const DynaPin::Pin pin{0, 0};
    const auto         blocker = DynaPin::blocker_for_pin(object, config, pin);
    const auto         surface = DynaPin::surface_for_pin(object, config, pin);
    CHECK(config.landing_edge_inset_y == 0.);
    CHECK(surface.edge_inset_y == 0.);
    const Point        shift   = object.instances().front().shift_without_plate_offset();
    REQUIRE_FALSE(blocker.poly.points.empty());
    CHECK(unscale_(blocker.poly.points.front().x() + shift.x()) == Catch::Approx(30.));
    CHECK(blocker.poly.points == surface.poly.points);
    config.pull_gcode.alternating.normal.y_origin_offset += 100.;
    config.pull_gcode.alternating.reverted.y_offset -= 100.;
    config.pull_gcode.alternating.normal.approach_z_offset += 100.;
    CHECK(DynaPin::blocker_for_pin(object, config, pin).poly.points == blocker.poly.points);
    CHECK(DynaPin::surface_for_pin(object, config, pin).poly.points == surface.poly.points);
}

TEST_CASE("DynaPin landing G-code interleaves overlapping model X ranges", "[Print][DynaPin][LandingOrder]")
{
    ResourcesDirGuard  resources_guard(test_resources_dir());
    TemporaryDirGuard  temporary_guard;
    DebugStageEnvGuard stage_guard;
    const std::string  support_type = GENERATE("normal(auto)", "tree(auto)");
    const std::string  layout       = GENERATE("models", "copies", "different_z", "disabled");
    const bool         shared       = layout == "models" || layout == "copies";
    PullConfigFile     file;
    file.write(pull_reference_fixtures()[0]["config"]);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"enable_dynapin_support_optimization", layout != "disabled"},
                                   {"dynapin_debug_stage", 1},
                                   {"dynapin_config_path", file.path.string()},
                                   {"dynapin_selected_pins", "0,0"},
                                   {"enable_support", true},
                                   {"support_type", support_type},
                                   {"support_style", "tree_slim"},
                                   {"initial_layer_speed", 20.},
                                   {"set_other_flow_ratios", true},
                                   {"gcode_flavor", "klipper"},
                                   {"exclude_object", true},
                                   {"gcode_label_objects", true},
                                   {"slow_down_layers", 0},
                                   {"slow_down_for_layer_cooling", "0"},
                                   {"filament_max_volumetric_speed", "1000"}});
    for (const std::string& key : config.keys()) {
        ConfigOption* option = config.option(key);
        if (option->type() == coEnums) {
            const auto* definition = print_config_def.get(key);
            if (option->nullable())
                static_cast<ConfigOptionEnumsGenericNullable*>(option)->keys_map = definition->enum_keys_map;
            else
                static_cast<ConfigOptionEnumsGeneric*>(option)->keys_map = definition->enum_keys_map;
        }
    }
    Print print;
    Model model;
    init_dynapin_print(print, model, config);
    if (layout == "copies") {
        model.objects.front()->add_instance()->set_offset({101., 130., 0.});
    } else {
        auto* second = model.add_object(*model.objects.front());
        second->name = "second.stl";
        second->instances.front()->set_offset({101., 130., 0.});
        second->config.set_key_value("support_flow_ratio", new ConfigOptionFloat(0.8));
    }
    print.apply(model, config);
    print.process();
    REQUIRE(print.num_object_instances() == 2);
    size_t expected_tracks = 0;
    double expected_volume = 0.;
    size_t object_index    = 0;
    for (auto* object : print.objects()) {
        REQUIRE(object->support_layers().empty());
        const double  z     = layout == "different_z" && object_index++ > 0 ? 7.7 : 7.5;
        auto*         layer = object->add_support_layer(0, 0, 0.2, z);
        const Polygon footprint(
            Points{Point::new_scale(-8., -8.), Point::new_scale(8., -8.), Point::new_scale(8., 8.), Point::new_scale(-8., 8.)});
        layer->support_islands         = {ExPolygon(footprint)};
        layer->dynapin_landing_regions = {footprint};
        const Flow flow(0.5, 0.2, 0.4);
        DynaPin::fill_landing_paths(layer->support_fills.entities, {footprint}, flow, 0.8, 0);
        ExtrusionPaths paths;
        DynaPin::collect_landing_paths(layer->support_fills, paths);
        for (const auto& path : paths) {
            expected_volume += path.total_volume() * object->instances().size() * object->config().support_flow_ratio.value;
            const auto& points = path.polyline.points;
            for (size_t i = 1; i < points.size(); ++i)
                expected_tracks += points[i - 1].x() == points[i].x() ? object->instances().size() : 0;
        }
    }
    const auto output_path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("dynapin-order-%%%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(output_path.string(), &result, nullptr);
    std::ifstream     stream(output_path.string());
    const std::string output((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    boost::filesystem::remove(output_path);
    const std::string role_prefix = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
    bool              support     = false;
    std::string       active_object;
    size_t            tracks = 0, owner_changes = 0;
    double            previous_x = -1e10, previous_y = -1e10, extrusion = 0.;
    double            previous_z = -1.;
    size_t            x_resets   = 0;
    GCodeReader       reader;
    reader.apply_config(print.config());
    reader.parse_buffer(output, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
        if (line.cmd_is("EXCLUDE_OBJECT_START")) {
            CHECK(active_object.empty());
            active_object = line.raw().substr(line.raw().find("NAME="));
        } else if (line.cmd_is("EXCLUDE_OBJECT_END")) {
            CHECK_FALSE(active_object.empty());
            CHECK(line.raw().substr(line.raw().find("NAME=")) == active_object);
            active_object.clear();
        }
        if (line.raw().find(role_prefix) == 0)
            support = line.raw() == role_prefix + "Support";
        if (!support || !line.cmd_is("G1") || !line.extruding(state) || (!line.has_x() && !line.has_y()))
            return;
        CHECK_FALSE(active_object.empty());
        CHECK(state.z() == Catch::Approx(layout == "different_z" && state.y() > 125. ? 7.7 : 7.5).margin(0.001));
        extrusion += line.dist_E(state);
        if (std::abs(line.new_X(state) - state.x()) < 0.001 && std::abs(line.new_Y(state) - state.y()) > 1.) {
            if (state.z() != previous_z)
                previous_x = -1e10;
            previous_z = state.z();
            if (shared)
                CHECK(state.x() >= previous_x - 0.001);
            x_resets += state.x() < previous_x - 0.001;
            CHECK(line.new_F(state) / 60. == Catch::Approx(20.).margin(0.01));
            ++tracks;
            owner_changes += previous_y > -1e9 && std::abs(state.y() - previous_y) > 20.;
            previous_x = state.x();
            previous_y = state.y();
        }
    });
    CHECK(tracks == expected_tracks);
    if (shared) {
        CHECK(owner_changes > 2);
        CHECK(x_resets == 0);
    } else
        CHECK(owner_changes <= 2);
    CHECK(active_object.empty());
    const double diameter = print.config().filament_diameter.get_at(0);
    CHECK(extrusion * PI * diameter * diameter / 4. == Catch::Approx(expected_volume).epsilon(0.002));
}

TEST_CASE("DynaPin slope bands preserve machine heights and landing connections", "[DynaPin][LandingSlope]")
{
    PullConfigFile file;
    file.write(pull_reference_fixtures()[1]["config"]);
    Print print;
    Model model;
    auto  runtime = DynamicPrintConfig::full_print_config();
    runtime.set_deserialize_strict({{"dynapin_config_path", file.path.string()}, {"printable_area", "0x0,180x0,180x180,0x180"}});
    init_dynapin_print(print, model, runtime);
    DynaPin::Config config;
    REQUIRE(DynaPin::load_config_for_print(print, config));
    const auto&        object = *print.objects().front();
    const DynaPin::Pin pin{1, 0};
    const auto         flat = DynaPin::surface_for_pin(object, config, pin);
    REQUIRE(DynaPin::landing_surfaces_for_pin(object, config, pin).size() == 1);
    CHECK(DynaPin::pin_pull_trigger_z(print, config, pin) == DynaPin::blocker_z_range(config, pin).z_max);
    config.landing_slope.slope_x = GENERATE(-1. / 150., 1. / 150.);
    const auto bands             = DynaPin::landing_surfaces_for_pin(object, config, pin);
    REQUIRE(bands.size() > 1);
    const auto envelope = DynaPin::surface_for_pin(object, config, pin);
    const auto shift    = object.instances().front().shift_without_plate_offset();
    Polygons   covered;
    for (const auto& band : bands) {
        const auto   box   = get_extents(band.poly);
        const double left  = unscale<double>(box.min.x() + shift.x());
        const double right = unscale<double>(box.max.x() + shift.x());
        CHECK(right - left <= config.landing_slope.max_band_width + EPSILON);
        CHECK(band.print_z ==
              Catch::Approx(std::max(DynaPin::pin_top_z_at_x(config, pin, left), DynaPin::pin_top_z_at_x(config, pin, right))));
        CHECK(band.print_z - std::min(DynaPin::pin_top_z_at_x(config, pin, left), DynaPin::pin_top_z_at_x(config, pin, right)) <=
              0.05 + EPSILON);
        CHECK(intersection(covered, {band.poly}).empty());
        covered.push_back(band.poly);
    }
    CHECK(diff({flat.poly}, covered).empty());
    CHECK(diff(covered, {flat.poly}).empty());
    CHECK(DynaPin::pin_top_z_at_x(config, pin, 175.) - DynaPin::pin_top_z_at_x(config, pin, 25.) ==
          Catch::Approx(config.landing_slope.slope_x * 150.));
    const std::vector<DynaPin::LandingLayer> source{{envelope.print_z + 0.1, envelope.print_z + 0.4, {flat.poly}}};
    const auto                               changes = DynaPin::align_landing_layers(source, bands, 0.08, 0.3);
    REQUIRE(changes.size() == bands.size());
    std::vector<DynaPin::LandingLayer> adjusted;
    for (size_t i = 0; i < changes.size(); ++i) {
        const auto& change = changes[i];
        CHECK(change.source == 0);
        CHECK(change.layers.front().bottom_z == Catch::Approx(bands[i].print_z));
        CHECK(change.layers.back().print_z == Catch::Approx(source.front().print_z));
        double bottom = bands[i].print_z;
        for (const auto& slab : change.layers) {
            CHECK(slab.bottom_z == Catch::Approx(bottom));
            CHECK(slab.print_z - slab.bottom_z >= 0.08 - EPSILON);
            CHECK(slab.print_z - slab.bottom_z <= 0.3 + EPSILON);
            bottom = slab.print_z;
            adjusted.push_back(slab);
        }
    }
    std::set<coord_t> shared_planes;
    for (const auto& layer : adjusted)
        shared_planes.insert(scale_(layer.print_z));
    // A roughly 1 mm wedge needs only a handful of shared planes, rather
    // than a separate family of intermediate Z values for every band.
    CHECK(shared_planes.size() <= 8);
    const auto bases = DynaPin::landing_base_layers(adjusted, bands, 2.);
    REQUIRE(bases.size() == bands.size());
    Polygons bases_covered;
    for (const auto& base : bases) {
        CHECK(intersection(bases_covered, base.polygons).empty());
        polygons_append(bases_covered, base.polygons);
    }
    CHECK(diff({flat.poly}, bases_covered).empty());
    // A narrow seed beside an internal boundary must expand onto both bands.
    const auto box      = get_extents(bands[bands.size() / 2].poly);
    auto       seed_box = box;
    seed_box.min.x()    = box.max.x() - scale_(0.3);
    seed_box.max.x()    = box.max.x() - scale_(0.1);
    seed_box.min.y() += scale_(3.);
    seed_box.max.y() -= scale_(3.);
    const std::vector<DynaPin::LandingLayer> narrow{{envelope.print_z + 0.1, envelope.print_z + 0.4, {seed_box.polygon()}}};
    const auto                               expanded = DynaPin::align_landing_layers(narrow, bands, 0.08, 0.3, 2.);
    CHECK(expanded.size() >= 2);
    Polygons expanded_region;
    for (const auto& change : expanded) {
        CHECK(intersection(expanded_region, change.layers.front().polygons).empty());
        polygons_append(expanded_region, change.layers.front().polygons);
    }
    const auto expected_region = intersection(offset(Polygons{seed_box.polygon()}, scale_(2.)), {envelope.poly});
    CHECK(diff(expected_region, expanded_region).empty());
    CHECK(diff(expanded_region, expected_region).empty());
    auto adjacent = DynaPin::landing_surfaces_for_pin(object, config, {pin.row, pin.col + 1});
    auto joined   = bands;
    joined.insert(joined.end(), adjacent.begin(), adjacent.end());
    auto edge_seed    = box;
    edge_seed.min.y() = box.max.y() - scale_(0.3);
    edge_seed.max.y() = box.max.y() - scale_(0.1);
    const std::vector<DynaPin::LandingLayer> edge_layer{{envelope.print_z + 0.1, envelope.print_z + 0.4, {edge_seed.polygon()}}};
    const auto                               across_pins = DynaPin::align_landing_layers(edge_layer, joined, 0.08, 0.3, 2.);
    Polygons                                 joined_region;
    for (const auto& change : across_pins)
        polygons_append(joined_region, change.layers.front().polygons);
    CHECK_FALSE(intersection(joined_region, {adjacent[bands.size() / 2].poly}).empty());

    CHECK(DynaPin::pin_pull_trigger_z(print, config, pin) <= bands.back().print_z + EPSILON);
    CHECK(DynaPin::pin_travel_clearance_z(print, config, {pin}) == Catch::Approx(envelope.print_z + config.landing_slope.travel_clearance));
}

TEST_CASE("DynaPin slope settings reject invalid numbers and reset when omitted", "[DynaPin][LandingSlope]")
{
    PullConfigFile file;
    Print          print;
    Model          model;
    init_pull_config_print(print, model, file);
    auto json             = pull_reference_fixtures()[0]["config"];
    json["landing_slope"] = {{"reference_x", 25.}, {"slope_x", -1. / 150.}, {"max_band_width", 7.5}};
    file.write(json);
    DynaPin::Config config;
    std::string     error;
    REQUIRE(DynaPin::load_config_for_print(print, config, &error));
    CHECK(config.landing_slope.slope_x == Catch::Approx(-1. / 150.));
    for (const auto& value : {nlohmann::json("nan"), nlohmann::json("bad"), nlohmann::json(nullptr)}) {
        json["landing_slope"]["slope_x"] = value;
        file.write(json);
        CHECK_FALSE(DynaPin::load_config_for_print(print, config, &error));
        CHECK(error.find("landing_slope.slope_x") != std::string::npos);
    }
    json["landing_slope"] = {{"max_band_width", 0.}};
    file.write(json);
    CHECK_FALSE(DynaPin::load_config_for_print(print, config, &error));
    json.erase("landing_slope");
    file.write(json);
    REQUIRE(DynaPin::load_config_for_print(print, config, &error));
    CHECK(config.landing_slope.slope_x == 0.);
}

TEST_CASE("DynaPin sloped landings generate lower support and pull before extrusion", "[DynaPin][LandingSlope][LandingSlopeIntegration]")
{
    ResourcesDirGuard  resources_guard(test_resources_dir());
    TemporaryDirGuard  temporary_guard;
    DebugStageEnvGuard stage_guard;
    const std::string  style  = GENERATE("default", "tree_slim", "tree_strong", "tree_hybrid");
    const bool         sloped = GENERATE(false, true);
    const bool         lifted = GENERATE(false, true);
    auto               json   = pull_reference_fixtures()[0]["config"];
    json["landing_slope"]     = {{"reference_x", 25.}, {"slope_x", sloped ? -1. / 150. : 0.}, {"max_band_width", 7.5}};
    PullConfigFile file;
    file.write(json);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"enable_support", true},
                                   {"enable_dynapin_support_optimization", true},
                                   {"dynapin_config_path", file.path.string()},
                                   {"dynapin_selected_pins", "1,0;1,1"},
                                   {"dynapin_debug_stage", 2},
                                   {"support_type", style == "default" ? "normal(auto)" : "tree(auto)"},
                                   {"support_style", style},
                                   {"raft_first_layer_expansion", 1.},
                                   {"raft_first_layer_density", 90.},
                                   {"printable_area", "0x0,180x0,180x180,0x180"},
                                   {"z_hop", lifted ? "0.2" : "0"},
                                   {"wipe", "1"},
                                   {"wipe_distance", "2"},
                                   {"retract_when_changing_layer", "1"},
                                   {"layer_change_gcode", ";AFTER_LAYER_CHANGE"},
                                   {"support_interface_bottom_layers", 0}});
    for (const auto& key : config.keys()) {
        auto* option = config.option(key);
        if (option->type() == coEnums) {
            const auto* definition = print_config_def.get(key);
            if (option->nullable())
                static_cast<ConfigOptionEnumsGenericNullable*>(option)->keys_map = definition->enum_keys_map;
            else
                static_cast<ConfigOptionEnumsGeneric*>(option)->keys_map = definition->enum_keys_map;
        }
    }
    Print print;
    Model model;
    print.is_BBL_printer() = false;
    auto* object = model.add_object();
    auto  mesh   = make_cube(5., 5., 5.);
    mesh.translate(30., 60., 0.);
    for (double x : {30., 95., 162.}) {
        auto deck = make_cube(8., 20., 2.);
        deck.translate(x, 8., 20.);
        mesh.merge(deck);
    }
    auto lower = make_cube(8., 8., 2.);
    lower.translate(95., 8., 6.);
    mesh.merge(lower);
    object->add_volume(std::move(mesh));
    object->add_instance();
    object->ensure_on_bed();
    print.set_plate_origin(Vec3d::Zero());
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    const auto& printed  = *print.objects().front();
    const auto  surfaces = DynaPin::landing_surfaces_for_object(printed);
    const auto  blockers = DynaPin::support_blocker_regions_local(printed);
    if (sloped)
        REQUIRE(surfaces.size() > 2);
    else
        REQUIRE(surfaces.size() == 2);
    REQUIRE(blockers.size() == surfaces.size());
    for (size_t i = 0; i < blockers.size(); ++i) {
        CHECK(blockers[i].z_max == surfaces[i].print_z);
        CHECK(blockers[i].z_max - blockers[i].z_min == Catch::Approx(5.));
    }
    std::set<int> positions;
    size_t        landings       = 0;
    double        support_volume = 0.;
    bool          lower_support  = false;
    for (const auto* layer : printed.support_layers()) {
        const auto flat    = layer->support_fills.flatten();
        auto       inspect = [&](const ExtrusionPath& path) {
            support_volume += path.total_volume();
            if (!path.is_dynapin_landing && layer->print_z < 6. - EPSILON)
                lower_support = true;
            if (!path.is_dynapin_landing)
                return;
            ++landings;
            const double bottom   = layer->print_z - path.height;
            const Point  midpoint = (path.first_point() + path.last_point()) / 2;
            bool         on_band  = false;
            for (const auto& surface : surfaces)
                if (surface.poly.contains(midpoint) && std::abs(bottom - surface.print_z) < 1e-4)
                    on_band = true;
            CHECK(on_band);
            const double x = unscale<double>(midpoint.x() + printed.instances().front().shift_without_plate_offset().x());
            positions.insert(x < 60. ? 0 : x < 130. ? 1 : 2);
            CHECK(path.height >= 0.08 - EPSILON);
            CHECK(path.height <= 0.4 + EPSILON);
        };
        for (const auto* entity : flat.entities) {
            if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity))
                inspect(*path);
            else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(entity))
                for (const auto& path : multi->paths)
                    inspect(path);
        }
    }
    CAPTURE(style, sloped, lifted, landings, support_volume);
    CHECK(landings > 0);
    CHECK(lower_support);
    CHECK(positions == std::set<int>{0, 1, 2});
    const auto           path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("dynapin-slope-%%%%.gcode");
    GCodeProcessorResult processor_result;
    print.export_gcode(path.string(), &processor_result, nullptr);
    std::ifstream     input(path.string());
    const std::string gcode((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    bool              pulled        = false;
    size_t            pulls         = 0;
    size_t            moves         = 0;
    bool              landing       = false;
    bool              in_pull_group = false;
    size_t            low_travels   = 0;
    DynaPin::Config   pin_config;
    REQUIRE(DynaPin::load_config_for_print(print, pin_config));
    const double clearance          = DynaPin::pin_travel_clearance_z(print, pin_config, {{1, 0}, {1, 1}});
    size_t       wipes              = 0;
    size_t       hop_checks         = 0;
    size_t       landing_travel_checks     = 0;
    size_t       continuous_landing_checks = 0;
    double       nominal_layer_z    = 0.;
    double       last_extrusion_z          = 0.;
    bool         last_extrusion_landing    = false;
    bool         moved_z_since_extrusion   = false;
    bool         wiping             = false;
    bool         first_layer_travel = false;
    GCodeReader  reader;
    reader.apply_config(print.config());
    reader.parse_buffer(gcode, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
        if (line.cmd_is("G1") && line.has_z() && std::abs(line.z() - state.z()) > 0.001)
            moved_z_since_extrusion = true;
        if (line.raw().rfind(";Z:", 0) == 0)
            nominal_layer_z = std::stod(line.raw().substr(3));
        if (line.raw().rfind("; Z_HEIGHT: ", 0) == 0)
            nominal_layer_z = std::stod(line.raw().substr(12));
        if (line.raw() == ";AFTER_LAYER_CHANGE")
            first_layer_travel = true;
        if (line.raw() == ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start)) {
            wiping = true;
            ++wipes;
        }
        if (line.raw() == ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End))
            wiping = false;
        if (!wiping && !in_pull_group && line.cmd_is("G1") && !line.has_e() && (line.has_x() || line.has_y()) && first_layer_travel) {
            if (sloped && lifted && pulled) {
                INFO(line.raw());
                CHECK(state.z() >= nominal_layer_z + 0.2 - 0.001);
                ++hop_checks;
            }
            first_layer_travel = false;
        }
        if (line.raw().find("; BEGIN_DYNAPIN_PULL") == 0) {
            ++pulls;
            pulled        = true;
            in_pull_group = true;
        }
        if (line.raw().find("; END_DYNAPIN_RETURN") == 0)
            in_pull_group = false;
        if (sloped && pulled && !in_pull_group && !wiping && line.cmd_is("G1") && !line.has_e() &&
            ((line.has_x() && std::abs(line.x() - state.x()) > 0.001) || (line.has_y() && std::abs(line.y() - state.y()) > 0.001))) {
            INFO(line.raw());
            const Point  from      = Point::new_scale(state.x(), state.y());
            const Point  to        = Point::new_scale(line.has_x() ? line.x() : state.x(), line.has_y() ? line.y() : state.y());
            const double minimum_z = line.has_z() ? std::min<double>(state.z(), line.z()) : state.z();
            CHECK(DynaPin::required_pin_travel_z(print, pin_config, {{1, 0}, {1, 1}}, Polyline(Points{from, to}), Vec2d::Zero(), minimum_z,
                                                 0.2) == 0.);
            if (landing) {
                CHECK((line.has_f() ? line.f() : state.f()) <= print.config().initial_layer_speed.value * 60. + 0.01);
                if (minimum_z >= nominal_layer_z + pin_config.landing_slope.travel_clearance - 0.001) {
                    CHECK_FALSE(line.has_z()); // Vertical release must precede XY.
                    ++landing_travel_checks;
                } else {
                    CHECK(last_extrusion_landing);
                    CHECK(last_extrusion_z == Catch::Approx(nominal_layer_z).margin(0.001));
                    CHECK(minimum_z == Catch::Approx(nominal_layer_z).margin(0.001));
                    CHECK(unscale<double>((to - from).norm()) < 2.);
                    CHECK_FALSE(moved_z_since_extrusion);
                    ++continuous_landing_checks;
                }
            }
            if (minimum_z < clearance - 0.001)
                ++low_travels;
        }
        if (line.raw() == DynaPin::LANDING_FAN_START)
            landing = true;
        if (line.raw() == DynaPin::LANDING_FAN_END)
            landing = false;
        if (landing && line.extruding(state)) {
            CHECK(pulled);
            CHECK(state.z() < 15.2);
            ++moves;
        }
        if (!wiping && line.extruding(state)) {
            last_extrusion_landing  = landing;
            last_extrusion_z        = line.has_z() ? line.z() : state.z();
            moved_z_since_extrusion = false;
        }
    });
    CHECK(pulls == 2);
    CHECK(moves > 0);
    CHECK((low_travels > 0) == sloped);
    CHECK(wipes > 0);
    if (sloped && lifted)
        CHECK(hop_checks > 0);
    if (sloped) {
        CHECK(landing_travel_checks > 0);
        CHECK(continuous_landing_checks > 0);
    }
    CAPTURE(print.print_statistics().total_used_filament, print.print_statistics().estimated_normal_print_time);
    CHECK(print.print_statistics().total_used_filament > 0.);
    boost::filesystem::remove(path);
}

TEST_CASE("DynaPin support removes collinear extrusion shared by separate instances", "[DynaPin][SupportOverlap]")
{
    const auto length = [](const ExtrusionEntityCollection& collection) {
        const auto flat   = collection.flatten();
        double     result = 0.;
        for (const auto* entity : flat.entities)
            result += entity->length();
        return result * SCALING_FACTOR;
    };
    const auto path = [](double x0, double y0, double x1, double y1) {
        ExtrusionPath result(erSupportMaterial, 0.1, 0.5, 0.2);
        result.polyline = Polyline(Points{Point::new_scale(x0, y0), Point::new_scale(x1, y1)});
        return result;
    };
    std::vector<DynaPin::LandingExtrusion> emitted;
    ExtrusionEntityCollection              first;
    first.append(path(0., 0., 0., 10.));
    DynaPin::remove_duplicate_support_paths(first, Point::new_scale(20., 30.), 0, erMixed, emitted);
    REQUIRE(first.entities.size() == 1);
    const bool                reversed = GENERATE(false, true);
    ExtrusionEntityCollection second;
    auto                      duplicate = reversed ? path(0., 10., 0., 0.) : path(0., 0., 0., 10.);
    duplicate.polyline.translate(Point::new_scale(10., 20.));
    second.append(duplicate);
    DynaPin::remove_duplicate_support_paths(second, Point::new_scale(10., 10.), 1, erMixed, emitted);
    CHECK(second.empty());

    ExtrusionEntityCollection partial;
    partial.append(path(20., 25., 20., 45.));
    DynaPin::remove_duplicate_support_paths(partial, Point(0, 0), 2, erMixed, emitted);
    CHECK(length(partial) == Catch::Approx(10.).margin(0.001));
    CHECK(partial.entities.size() == 2);

    ExtrusionEntityCollection distinct;
    distinct.append(path(19., 35., 21., 35.));     // transverse crossing
    distinct.append(path(20.01, 30., 20.01, 40.)); // separate parallel line
    auto thicker   = path(20., 30., 20., 40.);
    thicker.height = 0.3;
    distinct.append(thicker);
    DynaPin::remove_duplicate_support_paths(distinct, Point(0, 0), 3, erMixed, emitted);
    CHECK(length(distinct) == Catch::Approx(22.));

    ExtrusionEntityCollection same_source;
    same_source.append(path(0., 0., 0., 10.));
    DynaPin::remove_duplicate_support_paths(same_source, Point::new_scale(20., 30.), 0, erMixed, emitted);
    CHECK_FALSE(same_source.empty());

    emitted.clear();
    ExtrusionEntityCollection diagonal;
    diagonal.append(path(0., 0., 10., 10.));
    DynaPin::remove_duplicate_support_paths(diagonal, Point(0, 0), 0, erMixed, emitted);
    ExtrusionEntityCollection nested;
    auto*                     group  = new ExtrusionEntityCollection;
    auto                      marked = path(15., 15., 5., 5.);
    marked.is_dynapin_landing        = true;
    group->append(ExtrusionMultiPath(marked));
    nested.entities.push_back(group);
    DynaPin::remove_duplicate_support_paths(nested, Point(0, 0), 1, erMixed, emitted);
    CHECK(length(nested) == Catch::Approx(std::sqrt(50.)).margin(0.001));
    ExtrusionPaths landing;
    DynaPin::collect_landing_paths(nested, landing);
    REQUIRE(landing.size() == 1);
    CHECK(landing.front().first_point() == Point::new_scale(15., 15.));
    CHECK(landing.front().height == marked.height);
    CHECK(landing.front().mm3_per_mm == marked.mm3_per_mm);
}

TEST_CASE("DynaPin slope is inert when disabled and zero slope preserves G-code", "[DynaPin][LandingSlope][LandingSlopeCompatibility]")
{
    ResourcesDirGuard  resources_guard(test_resources_dir());
    TemporaryDirGuard  temporary_guard;
    DebugStageEnvGuard stage_guard;
    const bool         enabled = GENERATE(false, true);
    PullConfigFile     file;
    auto               json = pull_reference_fixtures()[0]["config"];
    file.write(json);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"dynapin_config_path", file.path.string()},
                                   {"enable_dynapin_support_optimization", enabled},
                                   {"dynapin_selected_pins", "0,0"},
                                   {"dynapin_debug_stage", 2},
                                   {"support_type", "normal(auto)"}});
    for (const auto& key : config.keys()) {
        auto* option = config.option(key);
        if (option->type() == coEnums) {
            const auto* definition = print_config_def.get(key);
            if (option->nullable())
                static_cast<ConfigOptionEnumsGenericNullable*>(option)->keys_map = definition->enum_keys_map;
            else
                static_cast<ConfigOptionEnumsGeneric*>(option)->keys_map = definition->enum_keys_map;
        }
    }
    Print prepared;
    Model model;
    prepared.is_BBL_printer() = false;
    init_dynapin_print(prepared, model, config);
    auto output = [&]() {
        Print print;
        print.is_BBL_printer() = false;
        print.set_plate_origin(Vec3d::Zero());
        print.apply(model, config);
        print.set_status_silent();
        print.process();
        print.get_object(size_t(0))->set_id(0);
        const auto           path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("dynapin-compat-%%%%.gcode");
        GCodeProcessorResult result;
        print.export_gcode(path.string(), &result, nullptr);
        std::ifstream input(path.string());
        std::string   text, line;
        while (std::getline(input, line))
            if (line.rfind("; generated by ", 0) != 0)
                text += line + '\n';
        boost::filesystem::remove(path);
        return text;
    };
    const auto baseline   = output();
    json["landing_slope"] = {{"slope_x", enabled ? 0. : -1. / 150.}, {"reference_x", 25.}, {"max_band_width", 7.5}};
    file.write(json);
    const auto actual = output();
    CAPTURE(enabled, actual.size(), baseline.size());
    CHECK((actual == baseline));
}

TEST_CASE("DynaPin only lifts travels crossing a raised physical pin surface", "[DynaPin][LandingSlope][PinTravel]")
{
    PullConfigFile file;
    file.write(pull_reference_fixtures()[0]["config"]);
    Print print;
    Model model;
    init_pull_config_print(print, model, file);
    DynaPin::Config config;
    REQUIRE(DynaPin::load_config_for_print(print, config));
    config.landing_slope.slope_x = -1. / 150.;
    const DynaPin::Pin pin{1, 0};
    const double       y = DynaPin::pin_y(config, pin) - 7.2;
    const double       z = DynaPin::pin_top_z_at_x(config, pin, 170.) + 0.25;
    const Polyline     root(Points{Point::new_scale(170., y), Point::new_scale(160., y)});
    const Polyline     across(Points{Point::new_scale(170., y), Point::new_scale(25., y)});
    auto               required = [&](const Polyline& path, double height, Vec2d origin = Vec2d::Zero()) {
        return DynaPin::required_pin_travel_z(print, config, {pin}, path, origin, height, 0.2);
    };
    CHECK(required(root, z) == 0.);
    CHECK(required(across, z) == Catch::Approx(DynaPin::pin_top_z_at_x(config, pin, 24.8) + 2.));
    CHECK(required(across, DynaPin::pin_top_z_at_x(config, pin, 20.) + 0.1) == 0.);
    auto local = across;
    local.translate(-scale_(10.), -scale_(50.));
    CHECK(required(local, z, Vec2d(10., 50.)) == required(across, z));
    auto outside = across;
    outside.translate(0, scale_(0.5 * config.blocker_width_y + 0.3));
    CHECK(required(outside, z) == 0.);
    auto edge = across;
    edge.translate(0, scale_(0.5 * config.blocker_width_y + 0.1));
    CHECK(required(edge, z) > z);
    CHECK(DynaPin::required_pin_travel_z(print, config, {}, across, Vec2d::Zero(), z, 0.2) == 0.);
    CHECK(DynaPin::required_pin_travel_z(print, config, {{0, 0}}, across, Vec2d::Zero(), z, 0.2) == 0.);
}

namespace {
void init_dynapin_geometry_print(Print& print, Model& model, const DynamicPrintConfig& config, bool bridge = false)
{
    TriangleMesh mesh = make_cube(5., 5., 20.);
    TriangleMesh top  = make_cube(60., 20., 5.);
    top.translate(0., 0., 20.);
    mesh.merge(top);
    if (bridge) {
        TriangleMesh second = make_cube(5., 20., 20.);
        second.translate(55., 0., 0.);
        mesh.merge(second);
    }
    auto* object = model.add_object();
    object->add_volume(std::move(mesh));
    object->add_instance()->set_offset({80., 40., 0.});
    object->ensure_on_bed();
    print.set_plate_origin(Vec3d::Zero());
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
}
} // namespace

TEST_CASE("DynaPin placement preparation skips only dispensable toolpaths", "[DynaPin][DynaPinGeometry]")
{
    ResourcesDirGuard  resources_guard(test_resources_dir());
    const bool         tree        = GENERATE(false, true);
    const bool         thick       = GENERATE(false, true);
    const bool         independent = GENERATE(false, true);
    DynamicPrintConfig config      = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"enable_support", true},
                                   {"enable_dynapin_support_optimization", true},
                                   {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
                                   {"support_type", tree ? "tree(auto)" : "normal(auto)"},
                                   {"thick_bridges", thick},
                                   {"independent_support_layer_height", independent}});
    Print print;
    Model model;
    init_dynapin_geometry_print(print, model, config);
    REQUIRE(print.prepare_slices_for_support_geometry({}, DynaPin::SupportGeometryMode::PlacementSearch));
    for (const PrintObject* object : print.objects()) {
        CHECK(object->is_step_done(posPerimeters));
        CHECK(object->is_step_done(posPrepareInfill));
        CHECK(object->is_step_done(posInfill) == (!tree && thick && independent));
        CHECK(object->step_state_with_timestamp(posEstimateCurledExtrusions).state == PrintStateBase::INVALID);
        CHECK(object->step_state_with_timestamp(posIroning).state == PrintStateBase::INVALID);
    }
}

TEST_CASE("DynaPin Normal search preparation preserves support geometry", "[DynaPin][DynaPinGeometry]")
{
    ResourcesDirGuard  resources_guard(test_resources_dir());
    const bool         bridge            = GENERATE(false, true);
    const bool         thick             = GENERATE(false, true);
    const bool         independent       = GENERATE(false, true);
    const bool         bridge_no_support = GENERATE(false, true);
    DynamicPrintConfig config            = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"enable_support", true},
                                   {"enable_dynapin_support_optimization", true},
                                   {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
                                   {"support_type", "normal(auto)"},
                                   {"thick_bridges", thick},
                                   {"independent_support_layer_height", independent},
                                   {"bridge_no_support", bridge_no_support}});
    Print exact_print, search_print;
    Model exact_model, search_model;
    init_dynapin_geometry_print(exact_print, exact_model, config, bridge);
    init_dynapin_geometry_print(search_print, search_model, config, bridge);
    std::vector<DynaPin::SupportSlab> exact, search;
    REQUIRE(exact_print.generate_support_geometry_only(exact));
    REQUIRE(
        search_print.generate_support_geometry_only(search, {}, nullptr, nullptr, nullptr, DynaPin::SupportGeometryMode::PlacementSearch));
    CHECK(DynaPin::support_volume_mm3(search) == Catch::Approx(DynaPin::support_volume_mm3(exact)).margin(1e-6));
    CHECK(search_print.dynapin_selection().pins == exact_print.dynapin_selection().pins);
}

TEST_CASE("DynaPin Tree geometry mode emits areas without extrusion paths", "[DynaPin][DynaPinGeometry]")
{
    ResourcesDirGuard  resources_guard(test_resources_dir());
    const char*        style  = GENERATE("tree_slim", "tree_strong", "tree_hybrid");
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"enable_support", true},
                                   {"enable_dynapin_support_optimization", true},
                                   {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
                                   {"support_type", "tree(auto)"},
                                   {"support_style", style}});
    Print print;
    Model model;
    init_dynapin_geometry_print(print, model, config);
    REQUIRE(print.prepare_slices_for_support_geometry({}, DynaPin::SupportGeometryMode::PlacementSearch));
    print.update_dynapin_selection();
    PrintObject* object = print.get_object(0);
    TreeSupport  tree(*object, object->slicing_parameters());
    tree.generate(TreeSupport::GenerationMode::GeometryOnly);
    REQUIRE_FALSE(object->support_layers().empty());
    double area = 0.;
    for (const SupportLayer* layer : object->support_layers()) {
        CHECK(layer->support_fills.empty());
        area += Slic3r::area(layer->base_areas);
    }
    CHECK(area > 0.);
}

TEST_CASE("DynaPin geometry benchmark compares bounded placement pipelines", "[.DynaPinGeometryBenchmark]")
{
    ResourcesDirGuard  resources_guard(test_resources_dir());
    TemporaryDirGuard  temporary_guard;
    DebugStageEnvGuard stage_guard;
    REQUIRE(::setenv("DYNAPIN_DEBUG_STAGE", "2", 1) == 0);
    const char* fixture = GENERATE("Simple_Bridge.3mf", "Arch_Bridge.3mf", "RemoveGymnastBase.3mf");
    if (const char* filter = std::getenv("DYNAPIN_BENCHMARK_FIXTURE"))
        if (std::string(filter) != fixture)
            return;
    const bool                exact = std::getenv("DYNAPIN_BENCHMARK_EXACT") != nullptr;
    const bool                full  = std::getenv("DYNAPIN_BENCHMARK_FULL") != nullptr;
    const auto                mode  = exact ? DynaPin::SupportGeometryMode::Exact : DynaPin::SupportGeometryMode::PlacementSearch;
    Model                     model;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext substitution{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs             plates;
    std::vector<Preset*>      presets;
    ImportedProjectDataGuard  imported(plates, presets);
    bool                      is_bbl = false;
    Semver                    version;
    const std::string         path = (boost::filesystem::path(TEST_DATA_DIR) / "dynapin" / fixture).string();
    REQUIRE(load_bbs_3mf(path.c_str(), &config, &substitution, &model, &plates, &presets, &is_bbl, &version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    config.set_deserialize_strict({{"enable_dynapin_support_optimization", true},
                                   {"enable_dynapin_placement_optimization", true},
                                   {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
                                   {"dynapin_debug_stage", 2},
                                   {"dynapin_selected_pins", ""},
                                   {"support_type", "tree(auto)"},
                                   {"printable_area", "0x0,180x0,180x180,0x180"},
                                   {"printable_height", 180.}});
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    for (auto* object : model.objects)
        print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    const auto preparation = DynaPin::prepare_placement_task(model, print, {{0., 0.}, {180., 0.}, {180., 180.}, {0., 180.}}, 180., {}, 0);
    REQUIRE(preparation.task);
    const auto allowed = [full](double angle) { return full || angle == 0. || angle == 5. || angle == 355.; };
    for (int repeat = 0; repeat < (full ? 1 : 3); ++repeat) {
        auto task = *preparation.task;
        if (!full)
            task.search.angle_group_concurrency = 1;
        // Default to a bounded search; full mode keeps production angles and concurrency.
        const auto range_provider              = task.search.delta_y_range_for_rotation;
        task.search.delta_y_range_for_rotation = [allowed, range_provider](double angle) {
            return allowed(angle) ? range_provider(angle) : std::optional<DynaPin::DeltaYInterval>{};
        };
        const auto evaluator        = DynaPin::make_scene_angle_evaluator(task.scene, {}, mode);
        task.search.angle_evaluator = [allowed, evaluator](double angle, const std::vector<DynaPin::PlacementCandidate>& candidates,
                                                           std::vector<std::optional<double>>&        results,
                                                           const DynaPin::CandidateCompletedCallback& completed) {
            if (allowed(angle))
                evaluator(angle, candidates, results, completed);
            else
                results.assign(candidates.size(), std::nullopt);
        };
        const auto start      = std::chrono::steady_clock::now();
        auto       result     = DynaPin::optimize_placement(task.search, DynaPin::make_scene_evaluator(task.scene, {}, mode));
        const auto search_end = std::chrono::steady_clock::now();
        if (!exact)
            result = DynaPin::revalidate_placement_result(std::move(result), {0., 0.}, DynaPin::make_scene_evaluator(task.scene));
        const double search_ms     = std::chrono::duration<double, std::milli>(search_end - start).count();
        const double validation_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - search_end).count();
        const double elapsed       = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "DYNAPIN_BENCH fixture=" << fixture << " exact=" << exact << " full=" << full << " repeat=" << repeat
                  << " elapsed_ms=" << elapsed << " search_ms=" << search_ms << " validation_ms=" << validation_ms
                  << " evaluations=" << result.evaluations << " status=" << int(result.status) << " volume=" << result.volume_mm3 << '\n';
        CHECK((result.status == DynaPin::PlacementStatus::Improved || result.status == DynaPin::PlacementStatus::Unchanged));
    }
}

TEST_CASE("DynaPin temporary placement evaluation preserves live geometry and Normal G-code", "[DynaPin][DynaPinGeometry]")
{
    ResourcesDirGuard  resources_guard(test_resources_dir());
    TemporaryDirGuard  temporary_guard;
    const bool         enabled = GENERATE(false, true);
    const bool         tree    = GENERATE(false, true);
    DynamicPrintConfig config  = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"enable_support", true},
                                   {"enable_dynapin_support_optimization", enabled},
                                   {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
                                   {"support_type", tree ? "tree(auto)" : "normal(auto)"},
                                   {"printable_area", "0x0,180x0,180x180,0x180"},
                                   {"printable_height", 180.},
                                   {"gcode_label_objects", false},
                                   {"seam_position", "back"}});
    for (const auto& key : config.keys()) {
        auto* option = config.option(key);
        if (option->type() == coEnums) {
            const auto* definition = print_config_def.get(key);
            if (option->nullable())
                static_cast<ConfigOptionEnumsGenericNullable*>(option)->keys_map = definition->enum_keys_map;
            else
                static_cast<ConfigOptionEnumsGeneric*>(option)->keys_map = definition->enum_keys_map;
        }
    }
    Print live;
    Model model;
    init_dynapin_geometry_print(live, model, config);
    DynaPin::PlacementSceneSnapshot scene;
    scene.model              = std::make_shared<Model>(model);
    scene.config             = config;
    const auto* object       = model.objects.front();
    const auto* instance     = object->instances.front();
    scene.target_object_id   = object->id();
    scene.target_instance_id = instance->id();
    scene.initial_transform  = instance->get_matrix();
    scene.world_center       = object->instance_bounding_box(*instance, false).center();
    scene.printable_area     = {{0., 0.}, {180., 0.}, {180., 180.}, {0., 180.}};
    scene.printable_height   = 180.;
    const auto export_gcode  = [](Print& print) {
        tbb::task_arena arena(1);
        return arena.execute([&] {
            std::srand(0);
            const auto path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("dynapin-geometry-%%%%-%%%%.gcode");
            print.process();
            GCodeProcessorResult result;
            print.export_gcode(path.string(), &result, nullptr);
            std::ifstream input(path.string());
            std::string   output((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            boost::filesystem::remove(path);
            return output;
        });
    };
    auto       before        = export_gcode(live);
    const auto support_paths = [&] {
        std::vector<Points> paths;
        for (const auto* object : live.objects())
            for (const auto* layer : object->support_layers())
                for (const auto& path : layer->support_fills.as_polylines())
                    paths.push_back(path.points);
        return paths;
    };
    const auto paths_before = support_paths();
    const auto pins_before  = live.dynapin_selection().pins;
    // Rejected candidates must leave the live Print unchanged too.
    DynaPin::evaluate_scene_candidate(scene, {0., 0.}, {}, DynaPin::SupportGeometryMode::PlacementSearch);
    CHECK(support_paths() == paths_before);
    CHECK(live.dynapin_selection().pins == pins_before);
    // Compare Tree's live paths directly; repeated exports are sensitive to
    // support path ordering and are not a stable byte-level oracle here.
    if (tree)
        return;
    auto after = export_gcode(live);
    // Only the generated-at comment is wall-clock dependent.
    const auto strip_timestamp = [](std::string& gcode) {
        const auto begin = gcode.find("; generated by ");
        if (begin != std::string::npos)
            gcode.erase(begin, gcode.find('\n', begin) - begin);
    };
    strip_timestamp(before);
    strip_timestamp(after);
    const auto mismatch = std::mismatch(before.begin(), before.end(), after.begin(), after.end());
    const auto offset   = size_t(mismatch.first - before.begin());
    INFO("First G-code difference at byte " << offset << ": " << before.substr(offset, 100) << " / "
                                            << after.substr(std::min(offset, after.size()), 100));
    CHECK(mismatch.first == before.end());
    CHECK(before.size() == after.size());
}
