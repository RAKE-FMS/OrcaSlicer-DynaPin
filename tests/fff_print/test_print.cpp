#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/DynaPin.hpp"
#include "libslic3r/DynaPinPlacement.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <boost/filesystem/path.hpp>

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

class ResourcesDirGuard
{
public:
    explicit ResourcesDirGuard(const std::string &path) : m_previous(resources_dir()) { set_resources_dir(path); }
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

void init_dynapin_print(Print& print, Model& model, const DynamicPrintConfig& config)
{
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
        }
    }
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
    for (const char* support_style : {"tree_slim", "tree_strong", "tree_hybrid"}) {
    Print print;
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
        {"dynapin_selected_pins", "2,0"},
        {"support_type", "tree(auto)"},
        {"support_style", support_style},
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
    }
}

TEST_CASE("DynaPin Tree landing targets the middle of a pin edge", "[DynaPin][Print]")
{
    Polygon safe_region;
    safe_region.points = {Point(scale_(0.), scale_(0.)), Point(scale_(40.), scale_(0.)),
                          Point(scale_(40.), scale_(4.)), Point(scale_(0.), scale_(4.))};
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
        Model model;
        TriangleMesh mesh = make_cube(10., 10., 5.);
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
        CHECK(dynapin_config.pull_gcode.x_front == Catch::Approx(20.));
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
