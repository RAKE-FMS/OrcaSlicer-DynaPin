#include <catch2/catch_all.hpp>

#include "libslic3r/DynaPin.hpp"
#include "libslic3r/DynaPinLanding.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/utils.hpp"

#include "test_data.hpp" // get access to init_print, etc

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <set>
#include <fstream>
#include <limits>

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
        {"dynapin_selected_pins", "5,0"},
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
        CHECK_FALSE(support_interface_at_z(print_object, 44.3));
    }
    THEN("The regular support interface remains above the pin top") {
        CHECK(support_interface_above_z(print_object, 44.3));
    }

    THEN("The first real support layer above the pin top has a regular interface") {
        CHECK(support_interface_at_z(print_object, 44.7));
    }

    THEN("The layer spanning the pin top is still generated") {
        CHECK(support_exists_at_z(print_object, 44.7));
    }

    THEN("Support from lower geometry remains below the selected pin body") {
        DynaPin::Config dynapin_config;
        REQUIRE(DynaPin::load_config_for_print(print, dynapin_config));
        const DynaPin::Pin pin{ 5, 0 };
        const DynaPin::VirtualSupportSurface surface = DynaPin::surface_for_pin(print_object, dynapin_config, pin);
        const DynaPin::LocalBlocker blocker = DynaPin::blocker_for_pin(print_object, dynapin_config, pin);
        CHECK(blocker.z_min == Catch::Approx(39.3));
        CHECK(blocker.z_max == Catch::Approx(44.3));
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
        {"dynapin_selected_pins", "5,0 6,0"},
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
    REQUIRE(print.dynapin_selection().pins == std::vector<DynaPin::Pin>{{5, 0}, {6, 0}});
    REQUIRE_FALSE(print_object.support_layers().empty());

    for (const DynaPin::Pin pin : { DynaPin::Pin{5, 0}, DynaPin::Pin{6, 0} }) {
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
    baseline_print.set_plate_origin(Vec3d::Zero());
    print.set_plate_origin(Vec3d::Zero());
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
        {"dynapin_selected_pins", "5,0"},
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
    const DynaPin::LocalBlocker blocker = DynaPin::blocker_for_pin(print_object, dynapin_config, { 5, 0 });
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

TEST_CASE("DynaPin landing interfaces follow bottom depth and reslicing", "[DynaPin][SupportMaterial][Cooling]")
{
    const char*       pattern = GENERATE("rectilinear", "concentric", "grid", "rectilinear_interlaced");
    ResourcesDirGuard resources_guard(test_resources_dir());
    DataDirGuard      data_dir_guard;
    Print             print;
    print.set_plate_origin(Vec3d::Zero());
    Model             model;
    TriangleMesh      mesh = Test::mesh(TestMesh::cube_20x20x20);
    TriangleMesh      slab = Test::mesh(TestMesh::cube_20x20x20);
    slab.scale(Vec3f(10.f, 5.f, 0.1f));
    slab.translate(0.f, 0.f, 50.f);
    mesh.merge(slab);
    ModelObject* object = model.add_object();
    object->add_volume(std::move(mesh));
    object->add_instance();
    object->ensure_on_bed();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"enable_dynapin_support_optimization", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", "5,0"},
        {"dynapin_debug_stage", 2},
        {"support_type", "normal(auto)"},
        {"support_angle", 37.},
        {"support_interface_pattern", pattern},
        {"support_interface_top_layers", 2},
        {"support_interface_bottom_layers", 2},
    });
    // Static enum-vector defaults lack the maps normally supplied by preset loading.
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
    print.auto_assign_extruders(object);
    auto landing_layers = [&]() {
        size_t count = 0;
        for (const SupportLayer* layer : print.objects().front()->support_layers()) {
            bool       has_landing = false;
            for (const auto* entity : layer->support_fills.entities) {
                const auto* ordered = dynamic_cast<const ExtrusionEntityCollection*>(entity);
                if (ordered == nullptr || !DynaPin::ordered_landing_collection(*ordered))
                    continue;
                coord_t previous_x = std::numeric_limits<coord_t>::lowest();
                for (const auto& polyline : ordered->as_polylines())
                    for (const Point& point : polyline.points) {
                        CHECK(point.x() >= previous_x);
                        previous_x = point.x();
                    }
            }
            const auto flat        = layer->support_fills.flatten();
            auto       check_path  = [&](const ExtrusionPath& path) {
                if (path.is_dynapin_landing) {
                    CHECK(path.role() == erSupportMaterialInterface);
                    has_landing = true;
                }
            };
            for (const ExtrusionEntity* entity : flat.entities) {
                if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity))
                    check_path(*path);
                else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(entity))
                    for (const auto& path : multipath->paths)
                        check_path(path);
                else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity))
                    for (const auto& path : loop->paths)
                        check_path(path);
            }
            count += has_landing;
        }
        return count;
    };
    for (const int bottom_layers : {0, 1, 2, 3, -1}) {
        CAPTURE(bottom_layers);
        config.set_deserialize_strict({{"support_interface_bottom_layers", bottom_layers}});
        print.apply(model, config);
        print.set_status_silent();
        print.process();
        CHECK(landing_layers() == (bottom_layers < 0 ? 2 : bottom_layers));
    }
    // Rotating/translating the model must not rotate the machine-axis raster.
    const Vec3d original_offset = object->instances.front()->get_offset();
    const Vec3d original_rotation = object->instances.front()->get_rotation();
    object->instances.front()->set_rotation(Vec3d(0., 0., PI / 2.));
    object->instances.front()->set_offset(Vec3d(150., 0., 0.));
    print.apply(model, config);
    print.process();
    CHECK(landing_layers() == 2);
    object->instances.front()->set_rotation(original_rotation);
    object->instances.front()->set_offset(original_offset);
    // Object overrides take precedence over the selected process preset.
    config.set_deserialize_strict({{"support_interface_bottom_layers", 0}});
    object->config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(3));
    print.apply(model, config);
    print.process();
    CHECK(landing_layers() == 3);
    object->config.erase("support_interface_bottom_layers");
    // Bottom depth remains meaningful even when ordinary top interfaces are disabled.
    config.set_deserialize_strict({{"support_interface_top_layers", 0}, {"support_interface_bottom_layers", 3}});
    print.apply(model, config);
    print.process();
    CHECK(landing_layers() == 3);
    config.set_deserialize_strict({
        {"support_material_interface_fan_speed", "100"},
        {"fan_speedup_time", 0.5},
        {"fan_kickstart", 0.3},
    });
    Print export_print;
    export_print.set_plate_origin(Vec3d::Zero());
    export_print.apply(model, config);
    export_print.set_status_silent();
    export_print.process();
    const auto output_path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("dynapin-cooling-%%%%%%.gcode");
    GCodeProcessorResult processor_result;
    export_print.export_gcode(output_path.string(), &processor_result, nullptr);
    std::ifstream     stream(output_path.string());
    const std::string output((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    boost::filesystem::remove(output_path);
    bool             protected_region = false;
    bool             ordinary_cooling = false;
    int              fan              = -1;
    std::set<double> protected_layers;
    double           previous_landing_z = -1.;
    double           previous_landing_x = -1.e20;
    GCodeReader      reader;
    reader.apply_config(export_print.config());
    reader.parse_buffer(output, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
        if (line.raw() == DynaPin::LANDING_FAN_START)
            protected_region = true;
        else if (line.raw() == DynaPin::LANDING_FAN_END)
            protected_region = false;
        float speed;
        if (line.cmd_is("M106") && !line.has('P') && line.has_value('S', speed))
            fan = int(speed);
        if (line.extruding(state)) {
            if (protected_region) {
                CHECK(fan == 0);
                if (std::abs(previous_landing_z - state.z()) > EPSILON) {
                    previous_landing_z = state.z();
                    previous_landing_x = -1.e20;
                }
                CHECK(state.x() >= previous_landing_x - 0.002);
                CHECK(line.new_X(state) >= state.x() - 0.002);
                previous_landing_x = line.new_X(state);
                protected_layers.insert(state.z());
            } else if (fan > 0)
                ordinary_cooling = true;
        }
    });
    CHECK_FALSE(protected_region);
    CHECK(protected_layers.size() == 3);
    CHECK(ordinary_cooling);
    config.set_deserialize_strict(
        {{"support_interface_top_layers", 2}, {"support_interface_bottom_layers", 64}, {"support_interface_filament", 1}});
    print.apply(model, config);
    print.process();
    double last_landing_z = 0.;
    for (const SupportLayer* layer : print.objects().front()->support_layers())
        if (!layer->dynapin_landing_regions.empty())
            last_landing_z = std::max(last_landing_z, layer->print_z);
    CHECK(last_landing_z > 49.6);
}
