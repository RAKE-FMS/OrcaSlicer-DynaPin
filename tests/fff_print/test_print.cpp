#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/DynaPin.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
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

SCENARIO("Print: Empty DynaPin selection warns for Tree supports", "[Print][DynaPin]") {
    GIVEN("Tree supports and no manual pin list") {
        ResourcesDirGuard resources_guard(test_resources_dir());
        Print print;
        Model model;
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"enable_support", true},
            {"enable_dynapin_support_optimization", true},
            {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
            {"dynapin_selected_pins", ""},
            {"support_type", "tree(auto)"},
        });
        init_dynapin_print(print, model, config);
        print.process();

        THEN("No pins are selected and a manual-selection warning is retained") {
            CHECK(print.dynapin_selection().pins.empty());
            CHECK(print.dynapin_selection().warning.find("Tree or Organic") != std::string::npos);
            CHECK(print.config().dynapin_selected_pins.value.empty());
        }
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
        top.translate(0.f, 0.f, 22.f);
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
            CHECK(std::any_of(print.dynapin_selection().pins.begin(), print.dynapin_selection().pins.end(),
                              [](const DynaPin::Pin &pin) { return pin.col == 2; }));
            CHECK(print.config().dynapin_selected_pins.value.empty());
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

SCENARIO("Print: Automatic DynaPin collision check covers the full pull path", "[Print][DynaPin]") {
    GIVEN("A model in the pin pull path but outside the pin body X range") {
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
        print.process();

        DynaPin::Config dynapin_config;
        REQUIRE(DynaPin::load_config_for_print(print, dynapin_config));

        THEN("The pin is rejected because its pull path intersects the model") {
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
            { "dynapin_selected_pins", "0,1" }
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
        REQUIRE(print.dynapin_selection().pins == std::vector<DynaPin::Pin>{{0, 1}});

        const auto first_blockers  = DynaPin::support_blocker_regions_local(*first_object);
        const auto second_blockers = DynaPin::support_blocker_regions_local(*second_object);
        REQUIRE(first_blockers.size() == 1);
        REQUIRE(second_blockers.size() == 1);
        CHECK(first_blockers.front().z_min == Catch::Approx(6.4));
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
            CHECK(boxes.front().pin == DynaPin::Pin{0, 1});
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
