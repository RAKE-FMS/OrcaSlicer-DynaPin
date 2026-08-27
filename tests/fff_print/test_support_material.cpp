#include <catch2/catch_all.hpp>

#include "libslic3r/DynaPin.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/utils.hpp"

#include "test_data.hpp" // get access to init_print, etc

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cmath>
#include <string>

using namespace Slic3r::Test;
using namespace Slic3r;

namespace {

class ResourcesDirGuard
{
public:
    explicit ResourcesDirGuard(const std::string &path) : m_previous(resources_dir()) { set_resources_dir(path); }
    ~ResourcesDirGuard() { set_resources_dir(m_previous); }

private:
    std::string m_previous;
};

class DataDirGuard
{
public:
    DataDirGuard() : m_previous(data_dir()), m_path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orcaslicer-dynapin-test-%%%%%%"))
    {
        boost::filesystem::create_directories(m_path / "SVG");
        set_data_dir(m_path.string());
    }

    ~DataDirGuard()
    {
        set_data_dir(m_previous);
        boost::filesystem::remove_all(m_path);
    }

private:
    std::string m_previous;
    boost::filesystem::path m_path;
};

std::string test_resources_dir()
{
    return (boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() / "resources").string();
}

bool support_interface_at_z(const PrintObject &object, double target_z)
{
    for (const SupportLayer *layer : object.support_layers()) {
        if (std::abs(layer->print_z - target_z) < 1e-4) {
            for (const ExtrusionEntity *entity : layer->support_fills.flatten().entities)
                if (entity->role() == erSupportMaterialInterface)
                    return true;
            return false;
        }
    }
    return false;
}

bool support_interface_above_z(const PrintObject &object, double lower_z)
{
    for (const SupportLayer *layer : object.support_layers()) {
        if (layer->print_z <= lower_z + 1e-4)
            continue;
        for (const ExtrusionEntity *entity : layer->support_fills.flatten().entities)
            if (entity->role() == erSupportMaterialInterface)
                return true;
    }
    return false;
}

bool support_exists_at_z(const PrintObject &object, double target_z)
{
    for (const SupportLayer *layer : object.support_layers())
        if (std::abs(layer->print_z - target_z) < 1e-4)
            return !layer->support_fills.empty();
    return false;
}

bool support_coverage_intersects_below_z(const PrintObject &object, const Polygon &region, double upper_z)
{
    for (const SupportLayer *layer : object.support_layers()) {
        if (layer->print_z >= upper_z - EPSILON)
            continue;

        Polygons covered;
        layer->support_fills.polygons_covered_by_width(covered, float(SCALED_EPSILON));
        if (!intersection(covered, { region }).empty())
            return true;
    }
    return false;
}

bool support_coverage_intersects_z_range(const PrintObject &object, const Polygon &region, double z_min, double z_max)
{
    for (const SupportLayer *layer : object.support_layers()) {
        if (layer->bottom_z() > z_max + EPSILON || layer->print_z + EPSILON < z_min)
            continue;

        Polygons covered;
        layer->support_fills.polygons_covered_by_width(covered, float(SCALED_EPSILON));
        if (!intersection(covered, { region }).empty())
            return true;
    }
    return false;
}

double maximum_support_coverage_area_in_z_range(const PrintObject &object, const Polygon &region, double z_min, double z_max)
{
    double maximum_area = 0.;
    for (const SupportLayer *layer : object.support_layers()) {
        if (layer->bottom_z() > z_max + EPSILON || layer->print_z + EPSILON < z_min)
            continue;

        Polygons covered;
        layer->support_fills.polygons_covered_by_width(covered, float(SCALED_EPSILON));
        maximum_area = std::max(maximum_area, area(intersection(covered, { region })));
    }
    return maximum_area;
}

} // namespace

SCENARIO("SupportMaterial: DynaPin keeps normal interfaces without a synthetic pin contact", "[SupportMaterial][DynaPin]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    DataDirGuard      data_dir_guard;

    TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
    TriangleMesh floating_slab = Test::mesh(TestMesh::cube_20x20x20);
    floating_slab.scale(Vec3f(10.f, 5.f, 0.1f));
    floating_slab.translate(0.f, 0.f, 50.f);
    mesh.merge(floating_slab);

    Print print;
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(std::move(mesh));
    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"enable_dynapin_support_optimization", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", "0,5"},
        {"dynapin_debug_stage", 2},
        {"support_type", "normal(auto)"},
        {"support_interface_top_layers", 2},
        {"support_interface_bottom_layers", 2},
    });
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();

    print.process();
    const PrintObject &print_object = *print.objects().front();

    THEN("No synthetic interface is printed at the pin top") {
        CHECK_FALSE(support_interface_at_z(print_object, 44.55));
    }
    THEN("The regular support interface remains above the pin top") {
        CHECK(support_interface_above_z(print_object, 44.55));
    }

    THEN("The first real support layer above the pin top has a regular interface") {
        CHECK(support_interface_at_z(print_object, 45.0));
    }

    THEN("The layer spanning the pin top is still generated") {
        CHECK(support_exists_at_z(print_object, 44.7));
    }

    THEN("Support from lower geometry remains below the selected pin body") {
        DynaPin::Config dynapin_config;
        REQUIRE(DynaPin::load_config_for_print(print, dynapin_config));
        const DynaPin::Pin pin{ 0, 5 };
        const DynaPin::VirtualSupportSurface surface = DynaPin::surface_for_pin(print_object, dynapin_config, pin);
        const DynaPin::LocalBlocker blocker = DynaPin::blocker_for_pin(print_object, dynapin_config, pin);
        CHECK(blocker.z_min == Catch::Approx(39.55));
        CHECK(blocker.z_max == Catch::Approx(44.55));
        CHECK(surface.print_z == Catch::Approx(blocker.z_max));
        // The pin must not globally erase support from lower geometry. This
        // coverage is supplied by the lower model contact, not by restarting
        // the upper floating-slab projection below the pin.
        CHECK(support_coverage_intersects_below_z(print_object, surface.poly, blocker.z_min));
    }
}

SCENARIO("SupportMaterial: DynaPin blocks stacked pin spans", "[SupportMaterial][DynaPin]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    DataDirGuard      data_dir_guard;

    TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
    TriangleMesh floating_slab = Test::mesh(TestMesh::cube_20x20x20);
    floating_slab.scale(Vec3f(10.f, 5.f, 0.1f));
    floating_slab.translate(0.f, 0.f, 60.f);
    mesh.merge(floating_slab);

    Print print;
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(std::move(mesh));
    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"enable_dynapin_support_optimization", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", "0,5 0,6"},
        {"dynapin_debug_stage", 2},
        {"support_type", "normal(auto)"},
        {"support_interface_top_layers", 2},
        {"support_interface_bottom_layers", 2},
    });
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    const PrintObject &print_object = *print.objects().front();
    DynaPin::Config dynapin_config;
    REQUIRE(DynaPin::load_config_for_print(print, dynapin_config));
    REQUIRE(print.dynapin_selection().pins == std::vector<DynaPin::Pin>{{0, 5}, {0, 6}});
    REQUIRE_FALSE(print_object.support_layers().empty());

    for (const DynaPin::Pin pin : { DynaPin::Pin{0, 5}, DynaPin::Pin{0, 6} }) {
        const DynaPin::LocalBlocker blocker = DynaPin::blocker_for_pin(print_object, dynapin_config, pin);
        CHECK(blocker.z_max > blocker.z_min);
        CHECK_FALSE(support_coverage_intersects_z_range(print_object, blocker.poly, blocker.z_min, blocker.z_max));
    }
}

SCENARIO("SupportMaterial: DynaPin propagates a connected blocker cutout downward", "[SupportMaterial][DynaPin]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    DataDirGuard      data_dir_guard;

    // Keep the bed-supported body away from the DynaPin corridor so the
    // assertion below observes only the floating upper support projection.
    TriangleMesh mesh = Test::mesh(TestMesh::cube_20x20x20);
    mesh.translate(-100.f, 0.f, 0.f);
    TriangleMesh floating_slab = Test::mesh(TestMesh::cube_20x20x20);
    floating_slab.scale(Vec3f(10.f, 5.f, 0.1f));
    floating_slab.translate(0.f, 0.f, 50.f);
    mesh.merge(floating_slab);

    Print baseline_print;
    Print print;
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(std::move(mesh));
    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig baseline_config = DynamicPrintConfig::full_print_config();
    baseline_config.set_deserialize_strict({
        {"enable_support", true},
        {"enable_dynapin_support_optimization", false},
        {"support_type", "normal(auto)"},
    });
    baseline_print.auto_assign_extruders(object);
    baseline_print.apply(model, baseline_config);
    baseline_print.validate();
    baseline_print.set_status_silent();
    baseline_print.process();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"enable_dynapin_support_optimization", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", "0,5"},
        {"dynapin_debug_stage", 2},
        {"support_type", "normal(auto)"},
    });
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    const PrintObject &print_object = *print.objects().front();
    DynaPin::Config dynapin_config;
    REQUIRE(DynaPin::load_config_for_print(print, dynapin_config));
    const DynaPin::LocalBlocker blocker = DynaPin::blocker_for_pin(print_object, dynapin_config, { 0, 5 });
    Polygon side_region = blocker.poly;
    side_region.translate(0., scaled<coord_t>(20.));
    const double baseline_area = maximum_support_coverage_area_in_z_range(
        *baseline_print.objects().front(), blocker.poly, blocker.z_min - 5., blocker.z_min - 0.05);
    const double optimized_area = maximum_support_coverage_area_in_z_range(
        print_object, blocker.poly, blocker.z_min - 5., blocker.z_min - 0.05);
    THEN("The connected support keeps the propagated blocker boundary below the pin") {
        CHECK(support_coverage_intersects_z_range(print_object, side_region, blocker.z_min - 5., blocker.z_min - 0.05));
        CHECK(optimized_area < baseline_area * 0.9);
    }
}

TEST_CASE("SupportMaterial: Three raft layers created", "[SupportMaterial][.]")
{
	Slic3r::Print print;
	Slic3r::Test::init_and_process_print({ TestMesh::cube_20x20x20 }, print, {
		{ "support_material", 1 },
		{ "raft_layers",      3 }
		});
    REQUIRE(print.objects().front()->support_layers().size() == 3);
}

SCENARIO("SupportMaterial: support_layers_z and contact_distance", "[SupportMaterial][.]")
{
    // Box h = 20mm, hole bottom at 5mm, hole height 10mm (top edge at 15mm).
    TriangleMesh mesh = Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_with_hole);
    mesh.rotate_x(float(M_PI / 2));
//    mesh.write_binary("d:\\temp\\cube_with_hole.stl");

	auto check = [](Slic3r::Print &print, bool &first_support_layer_height_ok, bool &layer_height_minimum_ok, bool &layer_height_maximum_ok, bool &top_spacing_ok)
	{
        ConstSupportLayerPtrsAdaptor support_layers = print.objects().front()->support_layers();

		first_support_layer_height_ok = support_layers.front()->print_z == print.config().initial_layer_print_height.value;

		layer_height_minimum_ok = true;
		layer_height_maximum_ok = true;
		double min_layer_height = print.config().min_layer_height.values.front();
		double max_layer_height = print.config().nozzle_diameter.values.front();
		if (print.config().max_layer_height.values.front() > EPSILON)
			max_layer_height = std::min(max_layer_height, print.config().max_layer_height.values.front());
		for (size_t i = 1; i < support_layers.size(); ++ i) {
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z < min_layer_height - EPSILON)
				layer_height_minimum_ok = false;
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z > max_layer_height + EPSILON)
				layer_height_maximum_ok = false;
		}

#if 0
		double expected_top_spacing = print.default_object_config().layer_height + print.config().nozzle_diameter.get_at(0);
		bool wrong_top_spacing = 0;
        std::vector<coordf_t> top_z { 1.1 };
		for (coordf_t top_z_el : top_z) {
			// find layer index of this top surface.
			size_t layer_id = -1;
			for (size_t i = 0; i < support_z.size(); ++ i) {
				if (abs(support_z[i] - top_z_el) < EPSILON) {
					layer_id = i;
					i = static_cast<int>(support_z.size());
				}
			}

			// check that first support layer above this top surface (or the next one) is spaced with nozzle diameter
			if (abs(support_z[layer_id + 1] - support_z[layer_id] - expected_top_spacing) > EPSILON && 
				abs(support_z[layer_id + 2] - support_z[layer_id] - expected_top_spacing) > EPSILON) {
				wrong_top_spacing = 1;
			}
		}
		d = ! wrong_top_spacing;
#else
		top_spacing_ok = true;
#endif
	};

    GIVEN("A print object having one modelObject") {
        WHEN("First layer height = 0.4") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
				{ "support_material",	1 },
				{ "layer_height",		0.2 },
				{ "first_layer_height", 0.4 },
                { "dont_support_bridges", false },
			});
			bool a, b, c, d;
            check(print, a, b, c, d);
            THEN("First layer height is honored")					{ REQUIRE(a == true); }
            THEN("No null or negative support layers")				{ REQUIRE(b == true); }
            THEN("No layers thicker than nozzle diameter")			{ REQUIRE(c == true); }
//            THEN("Layers above top surfaces are spaced correctly")	{ REQUIRE(d == true); }
        }
        WHEN("Layer height = 0.2 and, first layer height = 0.3") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
				{ "support_material",	1 },
				{ "layer_height",		0.2 },
				{ "first_layer_height", 0.3 },
                { "dont_support_bridges", false },
            });
            bool a, b, c, d;
            check(print, a, b, c, d);
            THEN("First layer height is honored")					{ REQUIRE(a == true); }
            THEN("No null or negative support layers")				{ REQUIRE(b == true); }
            THEN("No layers thicker than nozzle diameter")			{ REQUIRE(c == true); }
//            THEN("Layers above top surfaces are spaced correctly")	{ REQUIRE(d == true); }
        }
        WHEN("Layer height = nozzle_diameter[0]") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
				{ "support_material",	1 },
				{ "layer_height",		0.2 },
				{ "first_layer_height", 0.3 },
                { "dont_support_bridges", false },
            });
            bool a, b, c, d;
            check(print, a, b, c, d);
            THEN("First layer height is honored")					{ REQUIRE(a == true); }
            THEN("No null or negative support layers")				{ REQUIRE(b == true); }
            THEN("No layers thicker than nozzle diameter")			{ REQUIRE(c == true); }
//            THEN("Layers above top surfaces are spaced correctly")	{ REQUIRE(d == true); }
        }
    }
}

#if 0
// Test 8.
TEST_CASE("SupportMaterial: forced support is generated", "[SupportMaterial]")
{
    // Create a mesh & modelObject.
    TriangleMesh mesh = TriangleMesh::make_cube(20, 20, 20);

    Model model = Model();
    ModelObject *object = model.add_object();
    object->add_volume(mesh);
    model.add_default_instances();
    model.align_instances_to_origin();

    Print print = Print();

    std::vector<coordf_t> contact_z = {1.9};
    std::vector<coordf_t> top_z = {1.1};
    print.default_object_config.support_material_enforce_layers = 100;
    print.default_object_config.support_material = 0;
    print.default_object_config.layer_height = 0.2;
    print.default_object_config.set_deserialize("first_layer_height", "0.3");

    print.add_model_object(model.objects[0]);
    print.objects.front()->_slice();

    SupportMaterial *support = print.objects.front()->_support_material();
    auto support_z = support->support_layers_z(contact_z, top_z, print.default_object_config.layer_height);

    bool check = true;
    for (size_t i = 1; i < support_z.size(); i++) {
        if (support_z[i] - support_z[i - 1] <= 0)
            check = false;
    }

    REQUIRE(check == true);
}

// TODO
bool test_6_checks(Print& print)
{
	bool has_bridge_speed = true;

	// Pre-Processing.
	PrintObject* print_object = print.objects.front();
	print_object->infill();
	SupportMaterial* support_material = print.objects.front()->_support_material();
	support_material->generate(print_object);
	// TODO but not needed in test 6 (make brims and make skirts).

	// Exporting gcode.
	// TODO validation found in Simple.pm


	return has_bridge_speed;
}

// Test 6.
SCENARIO("SupportMaterial: Checking bridge speed", "[SupportMaterial]")
{
    GIVEN("Print object") {
        // Create a mesh & modelObject.
        TriangleMesh mesh = TriangleMesh::make_cube(20, 20, 20);

        Model model = Model();
        ModelObject *object = model.add_object();
        object->add_volume(mesh);
        model.add_default_instances();
        model.align_instances_to_origin();

        Print print = Print();
        print.config.brim_width = 0;
        print.config.skirts = 0;
        print.config.skirts = 0;
        print.default_object_config.support_material = 1;
        print.default_region_config.top_solid_layers = 0; // so that we don't have the internal bridge over infill.
        print.default_region_config.bridge_speed = 99;
        print.config.cooling = 0;
        print.config.set_deserialize("first_layer_speed", "100%");

        WHEN("support_material_contact_distance = 0.2") {
            print.default_object_config.support_material_contact_distance = 0.2;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);
            REQUIRE(check == true); // bridge speed is used.
        }

        WHEN("support_material_contact_distance = 0") {
            print.default_object_config.support_material_contact_distance = 0;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);
            REQUIRE(check == true); // bridge speed is not used.
        }

        WHEN("support_material_contact_distance = 0.2 & raft_layers = 5") {
            print.default_object_config.support_material_contact_distance = 0.2;
            print.default_object_config.raft_layers = 5;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);
            REQUIRE(check == true); // bridge speed is used.
        }

        WHEN("support_material_contact_distance = 0 & raft_layers = 5") {
            print.default_object_config.support_material_contact_distance = 0;
            print.default_object_config.raft_layers = 5;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);

            REQUIRE(check == true); // bridge speed is not used.
        }
    }
}

#endif
