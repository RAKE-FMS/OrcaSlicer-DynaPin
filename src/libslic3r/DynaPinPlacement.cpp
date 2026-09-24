#include "DynaPinPlacement.hpp"

#include "ClipperUtils.hpp"
#include "BuildVolume.hpp"
#include "Geometry.hpp"
#include "Layer.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "libslic3r.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_group.h>
#include <tbb/task_arena.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>

#include <boost/log/trivial.hpp>

namespace Slic3r::DynaPin {
namespace {

constexpr double angle_step_deg = 5.;
constexpr double tie_angle_epsilon = 1e-9;
constexpr double coordinate_epsilon = 1e-7;
constexpr uint64_t gib = uint64_t(1024) * 1024 * 1024;
constexpr uint64_t minimum_group_memory = gib / 2;

bool can_admit_angle_group(const SystemResourceSample &sample, uint64_t group_estimate, size_t active)
{
    if (sample.total_memory_bytes == 0 || group_estimate == 0)
        return false;
    const uint64_t reserve = std::max(uint64_t(2) * gib, sample.total_memory_bytes / 5);
    if (sample.available_memory_bytes <= reserve)
        return false;
    // Include active groups in the reservation, even though part of their
    // footprint may already be reflected in the current OS sample.
    const uint64_t slots = uint64_t(active) + 1;
    const uint64_t headroom = sample.available_memory_bytes - reserve;
    const uint64_t process_ceiling = sample.total_memory_bytes - sample.total_memory_bytes / 4;
    return slots <= headroom / group_estimate &&
           sample.process_resident_bytes < process_ceiling &&
           slots <= (process_ceiling - sample.process_resident_bytes) / group_estimate;
}

bool finite(double value) { return std::isfinite(value); }

double candidate_key_value(double value)
{
    return std::round(value / coordinate_epsilon) * coordinate_epsilon;
}

struct CandidateKey
{
    long long rotation;
    long long delta_y;

    bool operator<(const CandidateKey &rhs) const
    {
        return std::tie(rotation, delta_y) < std::tie(rhs.rotation, rhs.delta_y);
    }
};

CandidateKey candidate_key(const PlacementCandidate &candidate)
{
    return {static_cast<long long>(std::llround(normalized_rotation_deg(candidate.rotation_deg) / coordinate_epsilon)),
            static_cast<long long>(std::llround(candidate_key_value(candidate.delta_y) / coordinate_epsilon))};
}

void append_unique(std::vector<PlacementCandidate> &out, std::set<CandidateKey> &seen, PlacementCandidate candidate)
{
    candidate.rotation_deg = normalized_rotation_deg(candidate.rotation_deg);
    candidate.delta_y = candidate_key_value(candidate.delta_y);
    if (seen.emplace(candidate_key(candidate)).second)
        out.push_back(candidate);
}

bool candidate_tie_less(const PlacementCandidate &lhs,
                        const PlacementCandidate &rhs,
                        double                   current_rotation_deg)
{
    const double lhs_abs_y = std::abs(lhs.delta_y);
    const double rhs_abs_y = std::abs(rhs.delta_y);
    if (std::abs(lhs_abs_y - rhs_abs_y) > coordinate_epsilon)
        return lhs_abs_y < rhs_abs_y;

    const double lhs_angle = rotation_distance_deg(lhs.rotation_deg, current_rotation_deg);
    const double rhs_angle = rotation_distance_deg(rhs.rotation_deg, current_rotation_deg);
    if (std::abs(lhs_angle - rhs_angle) > tie_angle_epsilon)
        return lhs_angle < rhs_angle;

    const double lhs_normalized = normalized_rotation_deg(lhs.rotation_deg);
    const double rhs_normalized = normalized_rotation_deg(rhs.rotation_deg);
    if (std::abs(lhs_normalized - rhs_normalized) > tie_angle_epsilon)
        return lhs_normalized < rhs_normalized;
    return lhs.delta_y < rhs.delta_y;
}

bool strictly_better(double lhs_volume, double rhs_volume)
{
    return lhs_volume < rhs_volume && std::isfinite(lhs_volume);
}

std::optional<BuildVolume> build_volume_for_scene(const PlacementSceneSnapshot &scene)
{
    std::vector<Vec2d> area = scene.printable_area;
    if (area.empty()) {
        const ConfigOptionPoints *area_option = scene.config.opt<ConfigOptionPoints>("printable_area");
        if (area_option != nullptr)
            area = area_option->values;
    }

    double height = scene.printable_height;
    if (!finite(height)) {
        if (const ConfigOptionFloat *height_option = scene.config.opt<ConfigOptionFloat>("printable_height"); height_option != nullptr)
            height = height_option->value;
    }
    if (area.size() < 3 || !finite(height) || height < 0.)
        return std::nullopt;
    return BuildVolume(area, height, {}, {});
}

struct CandidateInstanceLayers
{
    const PrintObject *object = nullptr;
    const PrintInstance *instance = nullptr;
    Point shift{0, 0};
};

bool model_layers_overlap(const Print &print)
{
    std::vector<CandidateInstanceLayers> instances;
    for (const PrintObject *object : print.objects()) {
        if (object == nullptr)
            continue;
        for (const PrintInstance &instance : object->instances())
            instances.push_back({object, &instance, instance.shift_without_plate_offset()});
    }

    for (size_t lhs_idx = 0; lhs_idx < instances.size(); ++lhs_idx) {
        const CandidateInstanceLayers &lhs = instances[lhs_idx];
        for (size_t rhs_idx = lhs_idx + 1; rhs_idx < instances.size(); ++rhs_idx) {
            const CandidateInstanceLayers &rhs = instances[rhs_idx];
            if (lhs.instance == rhs.instance)
                continue;

            for (const Layer *lhs_layer : lhs.object->layers()) {
                if (lhs_layer == nullptr || lhs_layer->lslices.empty())
                    continue;
                const double lhs_bottom = lhs_layer->bottom_z();
                const double lhs_top = lhs_layer->print_z;
                for (const Layer *rhs_layer : rhs.object->layers()) {
                    if (rhs_layer == nullptr || rhs_layer->lslices.empty())
                        continue;
                    const double overlap_bottom = std::max(lhs_bottom, rhs_layer->bottom_z());
                    const double overlap_top = std::min(lhs_top, rhs_layer->print_z);
                    if (!(overlap_top - overlap_bottom > EPSILON))
                        continue;

                    ExPolygons lhs_slices = lhs_layer->lslices;
                    ExPolygons rhs_slices = rhs_layer->lslices;
                    for (ExPolygon &slice : lhs_slices)
                        slice.translate(lhs.shift);
                    for (ExPolygon &slice : rhs_slices)
                        slice.translate(rhs.shift);
                    if (!intersection(lhs_slices, rhs_slices).empty())
                        return true;
                }
            }
        }
    }
    return false;
}

bool model_intersects_bed_exclusion(const Print &print)
{
    const Polygons excluded = get_bed_excluded_area(print.config());
    if (excluded.empty() || excluded.front().empty())
        return false;

    for (const PrintObject *object : print.objects()) {
        if (object == nullptr)
            continue;
        for (const PrintInstance &instance : object->instances()) {
            const Point shift = instance.shift_without_plate_offset();
            for (const Layer *layer : object->layers()) {
                if (layer == nullptr || layer->lslices.empty())
                    continue;
                ExPolygons slices = layer->lslices;
                for (ExPolygon &slice : slices)
                    slice.translate(shift);
                if (!intersection(slices, excluded).empty())
                    return true;
            }
        }
    }
    return false;
}

} // namespace

bool PlacementEligibility::valid(std::string *error) const
{
    auto fail = [error](const char *message) {
        if (error)
            *error = message;
        return false;
    };
    if (!dynapin_enabled)
        return fail("DynaPin support optimization is disabled");
    if (!placement_enabled)
        return fail("DynaPin position/rotation optimization is disabled");
    if (!automatic_pin_selection)
        return fail("DynaPin placement optimization requires automatic pin selection");
    if (!support_configuration_eligible)
        return fail("DynaPin placement optimization requires Normal or non-Organic Tree support");
    if (selected_instance_count != 1)
        return fail("DynaPin placement optimization requires exactly one selected instance");
    return true;
}

PlacementResliceAction placement_reslice_action(bool optimization_reslice_guard,
                                                bool slice_result_valid,
                                                bool force_placement_search,
                                                bool worker_idle)
{
    if (optimization_reslice_guard || (slice_result_valid && !force_placement_search))
        return PlacementResliceAction::ContinueSlicing;
    if (!worker_idle)
        return PlacementResliceAction::WaitForWorker;
    return PlacementResliceAction::StartSearch;
}

bool placement_job_should_continue_reslice(PlacementStatus status, bool canceled, bool failed)
{
    return !canceled && !failed && status != PlacementStatus::Canceled && status != PlacementStatus::ResourceExhausted;
}

std::optional<DeltaYInterval> placement_delta_y_range_for_bbox(double bed_y_min,
                                                               double bed_y_max,
                                                               double bbox_min_y,
                                                               double bbox_max_y)
{
    if (!finite(bed_y_min) || !finite(bed_y_max) || !finite(bbox_min_y) || !finite(bbox_max_y) || bed_y_max < bed_y_min ||
        bbox_max_y < bbox_min_y)
        return std::nullopt;

    const double lower = bed_y_min - bbox_min_y;
    const double upper = bed_y_max - bbox_max_y;
    if (upper < lower)
        return std::nullopt;
    return DeltaYInterval{lower, upper};
}

double support_volume_mm3(const std::vector<SupportSlab> &slabs)
{
    std::vector<double> z_boundaries;
    z_boundaries.reserve(slabs.size() * 2);
    for (const SupportSlab &slab : slabs) {
        if (!finite(slab.z_min) || !finite(slab.z_max))
            throw std::invalid_argument("DynaPin support slab Z boundaries must be finite");
        if (slab.z_max < slab.z_min)
            throw std::invalid_argument("DynaPin support slab z_max must be >= z_min");
        if (slab.z_max == slab.z_min || slab.polygons.empty())
            continue;
        z_boundaries.push_back(slab.z_min);
        z_boundaries.push_back(slab.z_max);
    }
    if (z_boundaries.size() < 2)
        return 0.;

    std::sort(z_boundaries.begin(), z_boundaries.end());
    z_boundaries.erase(std::unique(z_boundaries.begin(), z_boundaries.end()), z_boundaries.end());

    long double volume = 0.;
    for (size_t i = 0; i + 1 < z_boundaries.size(); ++i) {
        const double z0 = z_boundaries[i];
        const double z1 = z_boundaries[i + 1];
        if (!(z1 > z0))
            continue;
        const double z_mid = z0 + (z1 - z0) * 0.5;
        Polygons active;
        for (const SupportSlab &slab : slabs)
            if (slab.z_min <= z_mid && z_mid < slab.z_max)
                polygons_append(active, slab.polygons);
        if (active.empty())
            continue;
        active = union_(active);
        volume += static_cast<long double>(area(active)) * static_cast<long double>(SCALING_FACTOR) * static_cast<long double>(SCALING_FACTOR) *
                  static_cast<long double>(z1 - z0);
    }
    return static_cast<double>(volume);
}

double support_volume_mm3(const Print &print)
{
    std::vector<SupportSlab> slabs;
    for (const PrintObject *object : print.objects()) {
        if (object == nullptr)
            continue;
        for (const SupportLayer *layer : object->support_layers()) {
            if (layer == nullptr || layer->support_islands.empty())
                continue;
            const double z_min = layer->print_z - layer->height;
            const double z_max = layer->print_z;
            if (!(z_max > z_min))
                continue;

            Polygons polygons;
            for (const ExPolygon &expolygon : layer->support_islands) {
                polygons.push_back(expolygon.contour);
                polygons_append(polygons, expolygon.holes);
            }

            // Support layers are stored in PrintObject-local coordinates.  A
            // PrintObject may represent several instances, so score all of
            // them in one plate coordinate system just like the placement
            // evaluator does.
            const PrintInstances &instances = object->instances();
            if (instances.empty()) {
                slabs.push_back({z_min, z_max, polygons});
            } else {
                for (const PrintInstance &instance : instances) {
                    SupportSlab slab{z_min, z_max, polygons};
                    for (Polygon &polygon : slab.polygons)
                        polygon.translate(instance.shift_without_plate_offset());
                    slabs.emplace_back(std::move(slab));
                }
            }
        }
    }
    return support_volume_mm3(slabs);
}

static const PrintObject* print_object_for_instance(const Print& print, const ObjectID& instance_id)
{
    for (const PrintObject* object : print.objects())
        if (object != nullptr &&
            std::any_of(object->instances().begin(), object->instances().end(), [&instance_id](const PrintInstance& instance) {
                return instance.model_instance != nullptr && instance.model_instance->id() == instance_id;
            }))
            return object;
    return nullptr;
}

static bool tree_candidate_requires_landing(const Print&            print,
                                            const ObjectID&         target_instance_id,
                                            const std::vector<Pin>& projected_tree_pins,
                                            const std::vector<Pin>& generated_tree_pins)
{
    const PrintObject* target = print_object_for_instance(print, target_instance_id);
    if (target == nullptr || !target->has_support() || !is_tree(target->config().support_type.value) ||
        is_tree_organic(target->config().support_type.value, target->config().support_style.value))
        return false;

    if (print.dynapin_selection().source != SelectionSource::Automatic)
        return false;

    // A zero-pin Tree candidate otherwise receives an ordinary Tree support
    // volume score and can beat candidates that preserve DynaPin support.
    // Keep such a pose out of the automatic placement search, just like a pose
    // whose projected pins fail to produce a usable landing.
    return projected_tree_pins.empty() || generated_tree_pins.empty();
}

std::optional<double> evaluate_scene_candidate(const PlacementSceneSnapshot &scene,
                                               const PlacementCandidate     &candidate,
                                               const CancelCallback         &cancel)
{
    if (!scene.model || !scene.target_object_id.valid() || !scene.target_instance_id.valid())
        return std::nullopt;
    if (cancel && cancel())
        return std::nullopt;

    Model candidate_model(*scene.model);
    ModelObject *target_object = nullptr;
    ModelInstance *target_instance = nullptr;
    for (ModelObject *object : candidate_model.objects) {
        if (object == nullptr || object->id() != scene.target_object_id)
            continue;
        target_object = object;
        for (ModelInstance *instance : object->instances)
            if (instance != nullptr && instance->id() == scene.target_instance_id) {
                target_instance = instance;
                break;
            }
        break;
    }
    if (target_object == nullptr || target_instance == nullptr)
        return std::nullopt;

    const std::optional<BuildVolume> build_volume = build_volume_for_scene(scene);
    if (!build_volume)
        return std::nullopt;

    target_instance->set_transformation(Geometry::Transformation(candidate_transform(
        scene.initial_transform, scene.world_center, candidate.rotation_deg, candidate.delta_y)));
    target_object->invalidate_bounding_box();

    // Recompute print-volume states after the candidate transform.  Print::apply
    // filters non-printable instances using this state, so a stale state could
    // silently omit an invalid candidate from the temporary print.
    candidate_model.curr_plate_index = scene.plate_index;
    candidate_model.update_print_volume_state(*build_volume);
    if (target_instance->calc_print_volume_state(*build_volume) != ModelInstancePVS_Inside)
        return std::nullopt;

    Print candidate_print;
    candidate_print.set_plate_index(scene.plate_index);
    candidate_print.set_plate_origin(scene.plate_origin);
    candidate_print.set_status_callback([&candidate_print, &cancel](const PrintBase::SlicingStatus &) {
        if (cancel && cancel())
            candidate_print.cancel();
    });
    candidate_print.apply(candidate_model, scene.config);
    if (cancel && cancel()) {
        candidate_print.cancel();
        return std::nullopt;
    }

    std::vector<SupportSlab> slabs;
    std::vector<Pin>         generated_tree_pins;
    std::vector<Pin>         projected_tree_pins;
    // This call runs update_dynapin_selection() before support generation.
    // The existing blocker collision scan therefore removes only colliding
    // pins while the candidate pose remains eligible for volume scoring.
    if (!candidate_print.generate_support_geometry_only(
            slabs, cancel, &scene.target_instance_id, &generated_tree_pins, &projected_tree_pins))
        return std::nullopt;
    if (cancel && cancel())
        return std::nullopt;
    if (tree_candidate_requires_landing(candidate_print, scene.target_instance_id, projected_tree_pins, generated_tree_pins))
        return std::nullopt;

    // Check all model instances in their actual Z intervals.  This catches
    // thin intersections between slices while allowing identical XY footprints
    // that are separated in Z.
    if (model_layers_overlap(candidate_print) || model_intersects_bed_exclusion(candidate_print))
        return std::nullopt;

    return support_volume_mm3(slabs);
}

PlacementEvaluator make_scene_evaluator(PlacementSceneSnapshot scene, const CancelCallback &cancel)
{
    return [scene = std::move(scene), cancel](const PlacementCandidate &candidate) {
        return evaluate_scene_candidate(scene, candidate, cancel);
    };
}

void evaluate_scene_angle_group(const PlacementSceneSnapshot          &scene,
                                double                                 rotation_deg,
                                const std::vector<PlacementCandidate> &candidates,
                                std::vector<std::optional<double>>    &results,
                                const CandidateCompletedCallback      &completed,
                                const CancelCallback                  &cancel)
{
    results.assign(candidates.size(), std::nullopt);
    if (candidates.empty())
        return;
    if (!scene.model || !scene.target_object_id.valid() || !scene.target_instance_id.valid())
        return;
    if (cancel && cancel())
        return;

    const std::optional<BuildVolume> build_volume = build_volume_for_scene(scene);
    if (!build_volume)
        return;

    Model          candidate_model(*scene.model);
    ModelObject   *target_object   = nullptr;
    ModelInstance *target_instance = nullptr;
    for (ModelObject *object : candidate_model.objects) {
        if (object == nullptr || object->id() != scene.target_object_id)
            continue;
        target_object = object;
        for (ModelInstance *instance : object->instances) {
            if (instance != nullptr && instance->id() == scene.target_instance_id) {
                target_instance = instance;
                break;
            }
        }
        break;
    }
    if (target_object == nullptr || target_instance == nullptr)
        return;

    candidate_model.curr_plate_index = scene.plate_index;

    std::vector<bool> classified(candidates.size(), false);
    size_t            initial_candidate = candidates.size();
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (cancel && cancel())
            return;
        target_instance->set_transformation(Geometry::Transformation(candidate_transform(
            scene.initial_transform, scene.world_center, rotation_deg, candidates[i].delta_y)));
        target_object->invalidate_bounding_box();
        candidate_model.update_print_volume_state(*build_volume);
        if (target_instance->calc_print_volume_state(*build_volume) == ModelInstancePVS_Inside) {
            initial_candidate = i;
            break;
        }
        classified[i] = true;
        if (completed)
            completed(i);
    }
    if (initial_candidate == candidates.size())
        return;

    Print candidate_print;
    candidate_print.set_plate_index(scene.plate_index);
    candidate_print.set_plate_origin(scene.plate_origin);
    candidate_print.set_status_callback([&candidate_print, &cancel](const PrintBase::SlicingStatus &) {
        if (cancel && cancel())
            candidate_print.cancel();
    });
    candidate_print.apply(candidate_model, scene.config);
    if (cancel && cancel()) {
        candidate_print.cancel();
        return;
    }

    if (!candidate_print.prepare_slices_for_support_geometry(cancel))
        return;
    if (cancel && cancel())
        return;

    PrintInstance *target_print_instance = nullptr;
    for (PrintObject *object : candidate_print.objects()) {
        if (object == nullptr)
            continue;
        for (PrintInstance &inst : object->instances()) {
            if (inst.model_instance != nullptr && inst.model_instance->id() == scene.target_instance_id) {
                target_print_instance = &inst;
                break;
            }
        }
        if (target_print_instance != nullptr)
            break;
    }
    if (target_print_instance == nullptr)
        return;

    ModelInstance *print_model_instance = const_cast<ModelInstance *>(target_print_instance->model_instance);
    ModelObject   *print_model_object   = print_model_instance == nullptr ? nullptr : print_model_instance->get_object();
    if (print_model_object == nullptr)
        return;

    for (size_t i = 0; i < candidates.size(); ++i) {
        if (classified[i])
            continue;
        if (cancel && cancel())
            return;

        const auto &candidate = candidates[i];

        target_instance->set_transformation(Geometry::Transformation(candidate_transform(
            scene.initial_transform, scene.world_center, candidate.rotation_deg, candidate.delta_y)));
        target_object->invalidate_bounding_box();

        candidate_model.update_print_volume_state(*build_volume);
        if (target_instance->calc_print_volume_state(*build_volume) != ModelInstancePVS_Inside) {
            if (completed)
                completed(i);
            continue;
        }

        const Transform3d instance_trafo = target_instance->get_transformation().get_matrix();
        print_model_instance->set_transformation(Geometry::Transformation(instance_trafo));
        print_model_instance->print_volume_state = target_instance->print_volume_state;
        print_model_object->invalidate_bounding_box();
        target_print_instance->shift = Point::new_scale(instance_trafo.data()[12], instance_trafo.data()[13]) +
                                       target_print_instance->print_object->center_offset();

        std::vector<SupportSlab> slabs;
        std::vector<Pin>         generated_tree_pins;
        std::vector<Pin>         projected_tree_pins;
        if (!candidate_print.generate_support_geometry_for_current_shift(
                slabs, cancel, &scene.target_instance_id, &generated_tree_pins, &projected_tree_pins)) {
            if (!(cancel && cancel()) && completed)
                completed(i);
            continue;
        }
        if (cancel && cancel())
            return;
        if (tree_candidate_requires_landing(candidate_print, scene.target_instance_id, projected_tree_pins, generated_tree_pins)) {
            if (completed)
                completed(i);
            continue;
        }

        if (model_layers_overlap(candidate_print) || model_intersects_bed_exclusion(candidate_print)) {
            if (completed)
                completed(i);
            continue;
        }

        results[i] = support_volume_mm3(slabs);
        if (completed)
            completed(i);
    }
}

AngleGroupEvaluator make_scene_angle_evaluator(PlacementSceneSnapshot scene, const CancelCallback &cancel)
{
    return [scene = std::move(scene), cancel](double                                 rotation_deg,
                                              const std::vector<PlacementCandidate> &candidates,
                                              std::vector<std::optional<double>>    &results,
                                              const CandidateCompletedCallback     &completed) {
        evaluate_scene_angle_group(scene, rotation_deg, candidates, results, completed, cancel);
    };
}

static PlacementPreparationResult prepare_placement_task_impl(const Model&                          model,
                                                              const Print&                          print,
                                                              const std::vector<Vec2d>&             printable_area,
                                                              double                                printable_height,
                                                              const Vec3d&                          plate_origin,
                                                              int                                   plate_index,
                                                              const std::optional<PlacementTarget>& preferred_target)
{
    PlacementPreparationResult preparation;
    const ModelObject*         target_object       = nullptr;
    const ModelInstance*       target_instance     = nullptr;
    const PrintObject*         target_print_object = nullptr;
    size_t                     printable_instances = 0;

    for (const PrintObject* print_object : print.objects()) {
        if (print_object == nullptr || print_object->model_object() == nullptr)
            continue;
        for (const PrintInstance& print_instance : print_object->instances()) {
            const ModelInstance* instance = print_instance.model_instance;
            if (instance == nullptr || instance->get_object() == nullptr)
                continue;
            ++printable_instances;
            const ModelObject* object    = instance->get_object();
            const bool         requested = preferred_target && object->id() == preferred_target->object_id &&
                                           instance->id() == preferred_target->instance_id;
            if (requested || (!preferred_target && target_instance == nullptr)) {
                target_object       = object;
                target_instance     = instance;
                target_print_object = print_object;
            }
        }
    }

    if (preferred_target) {
        if (target_instance == nullptr) {
            preparation.warning = "The requested DynaPin placement target is not printable on this plate";
            return preparation;
        }
    } else if (printable_instances != 1 || target_instance == nullptr) {
        preparation.warning = "DynaPin placement optimization requires exactly one printable instance";
        return preparation;
    }

    DynaPin::Config dynapin_config;
    if (!DynaPin::load_config_for_print(print, dynapin_config, &preparation.warning))
        return preparation;

    PlacementEligibility eligibility;
    eligibility.dynapin_enabled   = print.config().enable_dynapin_support_optimization.value && DynaPin::effective_debug_stage(print) >= 1;
    eligibility.placement_enabled = print.config().enable_dynapin_placement_optimization.value;
    eligibility.automatic_pin_selection = !DynaPin::has_manual_selection(print);
    eligibility.support_configuration_eligible = target_print_object != nullptr && target_print_object->has_support() &&
                                                 !is_tree_organic(target_print_object->config().support_type.value,
                                                                  target_print_object->config().support_style.value);
    eligibility.selected_instance_count = target_instance == nullptr ? 0 : 1;
    if (!eligibility.valid(&preparation.warning))
        return preparation;

    const BoundingBoxf3 initial_bbox = target_object->instance_bounding_box(*target_instance, false);
    if (!initial_bbox.defined) {
        preparation.warning = "The DynaPin placement target has no valid bounding box";
        return preparation;
    }
    if (printable_area.size() < 3 || !finite(printable_height)) {
        preparation.warning = "The DynaPin placement build volume is invalid";
        return preparation;
    }

    PlacementSceneSnapshot scene;
    scene.model              = std::make_shared<Model>(model);
    scene.config             = print.full_print_config();
    scene.target_object_id   = target_object->id();
    scene.target_instance_id = target_instance->id();
    scene.initial_transform  = target_instance->get_matrix();
    scene.world_center       = initial_bbox.center();
    scene.printable_area     = printable_area;
    scene.printable_height   = printable_height;
    scene.plate_origin       = plate_origin;
    scene.plate_index        = plate_index;

    double bed_y_min = std::numeric_limits<double>::infinity();
    double bed_y_max = -std::numeric_limits<double>::infinity();
    for (const Vec2d& point : printable_area) {
        bed_y_min = std::min(bed_y_min, point.y());
        bed_y_max = std::max(bed_y_max, point.y());
    }
    const std::optional<DeltaYInterval> y_range = placement_delta_y_range_for_bbox(bed_y_min, bed_y_max, initial_bbox.min.y(),
                                                                                   initial_bbox.max.y());
    if (!y_range) {
        preparation.warning = "The DynaPin placement target does not fit the printable Y range";
        return preparation;
    }

    PlacementSearchInput search;
    search.pitch_y                    = dynapin_config.col_pitch_y;
    search.y_min                      = y_range->min;
    search.y_max                      = y_range->max;
    search.current_rotation_deg       = 0.;
    search.current_delta_y            = 0.;
    search.current_volume_mm3         = std::numeric_limits<double>::quiet_NaN();
    search.angle_group_concurrency    = 0;
    search.delta_y_range_for_rotation = [scene](double rotation_deg) { return placement_delta_y_range_for_scene(scene, rotation_deg); };

    const bool tree_support = is_tree(target_print_object->config().support_type.value);

    preparation.task = PlacementTask{eligibility, std::move(search), std::move(scene), tree_support};
    return preparation;
}

PlacementPreparationResult prepare_placement_task(const Model&                          model,
                                                  const Print&                          print,
                                                  const std::vector<Vec2d>&             printable_area,
                                                  double                                printable_height,
                                                  const Vec3d&                          plate_origin,
                                                  int                                   plate_index,
                                                  const std::optional<PlacementTarget>& preferred_target)
{
    try {
        return prepare_placement_task_impl(model, print, printable_area, printable_height, plate_origin, plate_index, preferred_target);
    } catch (const std::bad_alloc&) {
        return {{}, "Insufficient memory while preparing DynaPin placement optimization"};
    } catch (const std::exception& error) {
        return {{}, std::string("Could not prepare DynaPin placement optimization: ") + error.what()};
    } catch (...) {
        return {{}, "Could not prepare DynaPin placement optimization"};
    }
}

PlacementResult run_placement_task(PlacementTask                task,
                                   const CancelCallback&        cancel,
                                   const ProgressCallback&      progress,
                                   const ProgressStageCallback& progress_stage)
{
    try {
        std::string eligibility_error;
        if (!task.eligibility.valid(&eligibility_error)) {
            PlacementResult result;
            result.status  = PlacementStatus::InvalidConfig;
            result.warning = std::move(eligibility_error);
            return result;
        }
        if (task.tree_support)
            task.search.angle_group_concurrency = 1;
        if (!task.search.angle_evaluator)
            task.search.angle_evaluator = make_scene_angle_evaluator(task.scene, cancel);
        return optimize_placement(task.search, make_scene_evaluator(task.scene, cancel), cancel, progress, progress_stage);
    } catch (const std::bad_alloc&) {
        PlacementResult result;
        result.status  = PlacementStatus::ResourceExhausted;
        result.warning = "Insufficient memory during DynaPin placement optimization";
        return result;
    } catch (const std::exception& error) {
        PlacementResult result;
        result.status  = PlacementStatus::NoFeasiblePose;
        result.warning = std::string("DynaPin placement optimization failed: ") + error.what();
        return result;
    } catch (...) {
        PlacementResult result;
        result.status  = PlacementStatus::NoFeasiblePose;
        result.warning = "DynaPin placement optimization failed";
        return result;
    }
}

PlacementApplyResult apply_placement_result(Model&                       model,
                                            const PlacementTask&         task,
                                            const PlacementResult&       result,
                                            const std::function<void()>& before_apply)
{
    PlacementApplyResult applied;
    if (result.status != PlacementStatus::Improved) {
        applied.warning = result.warning.empty() ? "DynaPin placement did not find a significant improvement" : result.warning;
        return applied;
    }

    ModelObject*   target_object   = nullptr;
    ModelInstance* target_instance = nullptr;
    for (ModelObject* object : model.objects) {
        if (object == nullptr || object->id() != task.scene.target_object_id)
            continue;
        target_object = object;
        for (ModelInstance* instance : object->instances)
            if (instance != nullptr && instance->id() == task.scene.target_instance_id) {
                target_instance = instance;
                break;
            }
        break;
    }
    if (target_object == nullptr || target_instance == nullptr) {
        applied.warning = "The DynaPin placement target no longer exists";
        return applied;
    }
    if (!target_instance->get_matrix().isApprox(task.scene.initial_transform, 1e-7)) {
        applied.warning = "The DynaPin placement target changed while the search was running";
        return applied;
    }

    const Geometry::Transformation candidate(candidate_transform(task.scene.initial_transform, task.scene.world_center,
                                                                 result.candidate.rotation_deg, result.candidate.delta_y));
    if (before_apply)
        before_apply();
    target_instance->set_transformation(candidate);
    target_object->invalidate_bounding_box();
    applied.applied = true;
    return applied;
}

std::vector<SupportSlab> generate_support_geometry(PrintObject &object)
{
    std::vector<SupportSlab> slabs;
    object.generate_support_geometry_only(slabs);
    return slabs;
}

std::optional<DeltaYInterval> placement_delta_y_interval(double lower, double upper, double pitch_y)
{
    if (!finite(lower) || !finite(upper) || !finite(pitch_y) || pitch_y == 0. || upper < lower)
        return std::nullopt;
    pitch_y = std::abs(pitch_y);
    const double width = upper - lower;
    if (width <= coordinate_epsilon)
        return DeltaYInterval{lower, upper};

    const double target = std::min(pitch_y, width);
    if (width <= pitch_y + coordinate_epsilon)
        return DeltaYInterval{lower, upper};

    double start = -0.5 * target;
    double end = 0.5 * target;
    if (start < lower) {
        start = lower;
        end = lower + target;
    }
    if (end > upper) {
        end = upper;
        start = upper - target;
    }
    start = std::max(start, lower);
    end = std::min(end, upper);
    return DeltaYInterval{start, end};
}

std::vector<PlacementCandidate> coarse_candidates(const DeltaYInterval &interval,
                                                   double               pitch_y,
                                                   double               current_rotation_deg,
                                                   double               current_delta_y)
{
    std::vector<PlacementCandidate> candidates;
    std::set<CandidateKey> seen;
    if (!finite(interval.min) || !finite(interval.max) || interval.max < interval.min || !finite(pitch_y) || pitch_y == 0.)
        return candidates;
    pitch_y = std::abs(pitch_y);

    const double step = pitch_y / 16.;
    for (int angle = 0; angle < 360; angle += static_cast<int>(angle_step_deg)) {
        for (int i = 0; ; ++i) {
            const double delta_y = interval.min + double(i) * step;
            if (delta_y > interval.max + coordinate_epsilon)
                break;
            append_unique(candidates, seen, {double(angle), delta_y});
        }
        append_unique(candidates, seen, {double(angle), interval.min});
        append_unique(candidates, seen, {double(angle), interval.max});
    }
    // The live pose is always evaluated separately.  It may be outside the
    // nominal one-pitch interval when the caller is resuming an already
    // shifted instance; the evaluator still decides whether that pose is
    // physically feasible.
    append_unique(candidates, seen, {current_rotation_deg, current_delta_y});
    return candidates;
}

static std::vector<PlacementCandidate> coarse_candidates_for_rotation_ranges(const PlacementSearchInput &input, double pitch_y)
{
    std::vector<PlacementCandidate> candidates;
    std::set<CandidateKey>          seen;
    const double                    step = pitch_y / 16.;

    std::vector<double> angles;
    for (int angle = 0; angle < 360; angle += static_cast<int>(angle_step_deg))
        angles.push_back(double(angle));

    std::vector<std::optional<DeltaYInterval>> intervals(angles.size());
    auto compute_intervals = [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            if (const std::optional<DeltaYInterval> feasible = input.delta_y_range_for_rotation(angles[i]))
                intervals[i] = placement_delta_y_interval(feasible->min, feasible->max, pitch_y);
        }
    };

    if (input.concurrency == 1) {
        compute_intervals(0, angles.size());
    } else {
        auto run_parallel = [&]() {
            tbb::parallel_for(tbb::blocked_range<size_t>(0, angles.size()), [&](const tbb::blocked_range<size_t> &range) {
                compute_intervals(range.begin(), range.end());
            });
        };
        if (input.concurrency > 1) {
            tbb::task_arena arena(static_cast<int>(input.concurrency));
            arena.execute(run_parallel);
        } else {
            run_parallel();
        }
    }

    for (size_t a = 0; a < angles.size(); ++a) {
        const auto &interval = intervals[a];
        if (!interval)
            continue;
        const double angle = angles[a];
        for (int i = 0;; ++i) {
            const double delta_y = interval->min + double(i) * step;
            if (delta_y > interval->max + coordinate_epsilon)
                break;
            append_unique(candidates, seen, {angle, delta_y});
        }
        append_unique(candidates, seen, {angle, interval->min});
        append_unique(candidates, seen, {angle, interval->max});
    }

    append_unique(candidates, seen, {input.current_rotation_deg, input.current_delta_y});
    return candidates;
}

std::vector<PlacementCandidate> local_candidates(const PlacementCandidate &seed, double pitch_y)
{
    std::vector<PlacementCandidate> candidates;
    std::set<CandidateKey> seen;
    if (!finite(seed.rotation_deg) || !finite(seed.delta_y) || !finite(pitch_y) || pitch_y == 0.)
        return candidates;
    pitch_y = std::abs(pitch_y);

    const double delta_step = pitch_y / 64.;
    for (int angle_step = -4; angle_step <= 4; ++angle_step)
        for (int delta_step_count = -4; delta_step_count <= 4; ++delta_step_count)
            append_unique(candidates, seen,
                          {seed.rotation_deg + double(angle_step), seed.delta_y + double(delta_step_count) * delta_step});
    return candidates;
}

double normalized_rotation_deg(double rotation_deg)
{
    if (!finite(rotation_deg))
        return rotation_deg;
    double result = std::fmod(rotation_deg, 360.);
    if (result < 0.)
        result += 360.;
    if (std::abs(result - 360.) < tie_angle_epsilon)
        result = 0.;
    return result;
}

double rotation_distance_deg(double lhs_deg, double rhs_deg)
{
    if (!finite(lhs_deg) || !finite(rhs_deg))
        return std::numeric_limits<double>::infinity();
    const double difference = std::abs(normalized_rotation_deg(lhs_deg) - normalized_rotation_deg(rhs_deg));
    return std::min(difference, 360. - difference);
}

Transform3d candidate_transform(const Transform3d &initial,
                                const Vec3d       &world_center,
                                double             rotation_deg,
                                double             delta_y)
{
    const double radians = rotation_deg * PI / 180.;
    const Transform3d rotation(Eigen::AngleAxisd(radians, Vec3d::UnitZ()));
    Transform3d       result = Transform3d::Identity();
    result = Eigen::Translation3d(Vec3d(0., delta_y, 0.)) * Eigen::Translation3d(world_center) * rotation *
             Eigen::Translation3d(-world_center) * initial;
    return result;
}

std::optional<DeltaYInterval> placement_delta_y_range_for_scene(const PlacementSceneSnapshot &scene, double rotation_deg)
{
    if (!scene.model || !finite(rotation_deg) || scene.printable_area.size() < 3)
        return std::nullopt;

    const ModelObject   *target_object   = nullptr;
    const ModelInstance *target_instance = nullptr;
    for (const ModelObject *object : scene.model->objects) {
        if (object == nullptr || object->id() != scene.target_object_id)
            continue;
        target_object = object;
        for (const ModelInstance *instance : object->instances)
            if (instance != nullptr && instance->id() == scene.target_instance_id) {
                target_instance = instance;
                break;
            }
        break;
    }
    if (target_object == nullptr || target_instance == nullptr)
        return std::nullopt;

    const Transform3d inst_matrix = candidate_transform(scene.initial_transform, scene.world_center, rotation_deg, 0.);
    BoundingBoxf3     bbox;
    for (const ModelVolume *v : target_object->volumes) {
        if (v != nullptr && v->is_model_part())
            bbox.merge(v->mesh().transformed_bounding_box(inst_matrix * v->get_matrix()));
    }
    if (!bbox.defined)
        return std::nullopt;

    double bed_y_min = std::numeric_limits<double>::infinity();
    double bed_y_max = -std::numeric_limits<double>::infinity();
    for (const Vec2d &point : scene.printable_area) {
        bed_y_min = std::min(bed_y_min, point.y());
        bed_y_max = std::max(bed_y_max, point.y());
    }
    return placement_delta_y_range_for_bbox(bed_y_min, bed_y_max, bbox.min.y(), bbox.max.y());
}

PlacementResult optimize_placement(const PlacementSearchInput &input,
                                   const PlacementEvaluator   &evaluator,
                                   const CancelCallback        &cancel,
                                   const ProgressCallback      &progress,
                                   const ProgressStageCallback &progress_stage)
{
    PlacementResult result;
    std::mutex      progress_mutex;
    int             last_progress = -1;
    bool            progress_finished = false;
    auto set_progress = [&](int value) {
        const int clamped = std::max(0, std::min(100, value));
        std::lock_guard<std::mutex> lock(progress_mutex);
        if (progress_finished || clamped <= last_progress)
            return;
        last_progress = clamped;
        if (progress)
            progress(clamped);
        if (clamped == 100)
            progress_finished = true;
    };
    auto set_stage = [&progress_stage](PlacementProgressStage stage) {
        if (progress_stage)
            progress_stage(stage);
    };
    set_stage(PlacementProgressStage::EvaluatingCurrent);
    set_progress(0);

    const bool has_rotation_ranges = bool(input.delta_y_range_for_rotation);
    const bool invalid_fixed_range = !has_rotation_ranges &&
                                     (!finite(input.y_min) || !finite(input.y_max) || input.y_max < input.y_min);
    if (!evaluator || !finite(input.pitch_y) || input.pitch_y == 0. || invalid_fixed_range ||
        !finite(input.current_rotation_deg) || !finite(input.current_delta_y)) {
        result.status = PlacementStatus::InvalidConfig;
        set_progress(100);
        return result;
    }

    const double pitch_y = std::abs(input.pitch_y);
    if (cancel && cancel()) {
        result.status = PlacementStatus::Canceled;
        set_progress(100);
        return result;
    }

    const PlacementCandidate current{normalized_rotation_deg(input.current_rotation_deg), input.current_delta_y};
    std::optional<double> current_volume;
    try {
        ++result.evaluations;
        current_volume = evaluator(current);
    } catch (const std::bad_alloc &) {
        result.status = PlacementStatus::ResourceExhausted;
        result.warning = "Insufficient memory for DynaPin placement search";
        set_progress(100);
        return result;
    } catch (const std::exception &error) {
        if (cancel && cancel()) {
            result.status = PlacementStatus::Canceled;
            set_progress(100);
            return result;
        }
        result.status = PlacementStatus::NoFeasiblePose;
        result.warning = error.what();
        set_progress(100);
        return result;
    } catch (...) {
        if (cancel && cancel()) {
            result.status = PlacementStatus::Canceled;
            set_progress(100);
            return result;
        }
        result.status = PlacementStatus::NoFeasiblePose;
        result.warning = "DynaPin candidate evaluation failed";
        set_progress(100);
        return result;
    }
    // A scene evaluator can observe cancellation while preparing the current
    // pose and report that pose as rejected.  Preserve the cancellation
    // result instead of misclassifying it as an infeasible initial pose.
    if (cancel && cancel()) {
        result.status = PlacementStatus::Canceled;
        set_progress(100);
        return result;
    }
    if (!current_volume || !finite(*current_volume) || *current_volume < 0.) {
        result.status = PlacementStatus::NoFeasiblePose;
        result.warning = "The current pose is not feasible";
        set_progress(100);
        return result;
    }
    set_progress(2);

    const double baseline_volume = finite(input.current_volume_mm3) && input.current_volume_mm3 >= 0. ? input.current_volume_mm3 : *current_volume;
    const double epsilon = std::max(1., baseline_volume * 0.001);
    set_stage(PlacementProgressStage::PreparingCandidates);
    std::vector<PlacementCandidate> coarse;
    if (has_rotation_ranges) {
        coarse = coarse_candidates_for_rotation_ranges(input, pitch_y);
    } else {
        const std::optional<DeltaYInterval> interval = placement_delta_y_interval(input.y_min, input.y_max, pitch_y);
        if (!interval) {
            result.status = PlacementStatus::InvalidConfig;
            set_progress(100);
            return result;
        }
        coarse = coarse_candidates(*interval, pitch_y, input.current_rotation_deg, input.current_delta_y);
    }
    set_progress(5);
    std::vector<std::pair<PlacementCandidate, double>> feasible;
    feasible.reserve(coarse.size());
    std::set<CandidateKey> evaluated;
    evaluated.emplace(candidate_key(current));
    feasible.emplace_back(current, *current_volume);

    std::atomic<bool> canceled{false};
    std::atomic<bool> resource_exhausted{false};
    auto check_canceled = [&]() {
        if (resource_exhausted.load(std::memory_order_relaxed))
            return true;
        if (canceled.load(std::memory_order_relaxed))
            return true;
        if (!cancel)
            return false;
        if (cancel()) {
            canceled.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    };

    auto evaluate_candidates = [&](const std::vector<PlacementCandidate> &candidates,
                                   int                                    phase_start,
                                   int                                    phase_end,
                                   bool                                   parallel) {
        std::vector<std::optional<double>> values(candidates.size());
        std::vector<unsigned char>         completed_candidates(candidates.size(), 0);
        size_t                             completed_count = 0;

        auto complete_candidate = [&](size_t index) {
            if (index >= candidates.size())
                return;
            std::lock_guard<std::mutex> lock(progress_mutex);
            if (completed_candidates[index] != 0)
                return;
            completed_candidates[index] = 1;
            ++completed_count;
            ++result.evaluations;
            const long long progress_span = static_cast<long long>(phase_end - phase_start);
            const int next_progress = phase_start + static_cast<int>(
                (progress_span * static_cast<long long>(completed_count)) / static_cast<long long>(candidates.size()));
            if (!progress_finished && next_progress > last_progress) {
                last_progress = next_progress;
                if (progress)
                    progress(next_progress);
            }
        };

        if (candidates.empty()) {
            set_progress(phase_end);
            return values;
        }

        if (input.angle_evaluator) {
            struct AngleCandidateItem {
                size_t             index;
                PlacementCandidate candidate;
            };
            std::map<double, std::vector<AngleCandidateItem>> angle_groups_map;
            for (size_t i = 0; i < candidates.size(); ++i) {
                const double angle = normalized_rotation_deg(candidates[i].rotation_deg);
                angle_groups_map[angle].push_back({i, candidates[i]});
            }

            std::vector<std::pair<double, std::vector<AngleCandidateItem>>> angle_groups;
            angle_groups.reserve(angle_groups_map.size());
            for (auto &pair : angle_groups_map)
                angle_groups.emplace_back(pair.first, std::move(pair.second));

            auto process_group = [&](size_t g) {
                if (check_canceled())
                    return;
                const double &angle = angle_groups[g].first;
                const auto   &items = angle_groups[g].second;
                std::vector<PlacementCandidate> group_candidates;
                group_candidates.reserve(items.size());
                for (const auto &item : items)
                    group_candidates.push_back(item.candidate);

                std::vector<std::optional<double>> group_results;
                std::vector<unsigned char>         group_completed(items.size(), 0);
                std::mutex                         group_completion_mutex;
                auto complete_group_candidate = [&](size_t index) {
                    if (index >= items.size())
                        return;
                    {
                        std::lock_guard<std::mutex> lock(group_completion_mutex);
                        if (group_completed[index] != 0)
                            return;
                        group_completed[index] = 1;
                    }
                    complete_candidate(items[index].index);
                };
                try {
                    input.angle_evaluator(angle, group_candidates, group_results, complete_group_candidate);
                } catch (const std::bad_alloc &) {
                    throw;
                } catch (...) {
                }

                for (size_t i = 0; i < items.size(); ++i) {
                    if (i < group_results.size())
                        values[items[i].index] = group_results[i];
                }
                if (!check_canceled())
                    for (size_t i = 0; i < items.size(); ++i)
                        complete_group_candidate(i);
            };
            auto safe_process_group = [&](size_t g) {
                try {
                    process_group(g);
                } catch (const std::bad_alloc &) {
                    resource_exhausted.store(true, std::memory_order_relaxed);
                }
            };

            if (!parallel || input.angle_group_concurrency == 1) {
                for (size_t g = 0; g < angle_groups.size(); ++g)
                    safe_process_group(g);
            } else if (input.angle_group_concurrency > 1) {
                const size_t worker_count = std::min(input.angle_group_concurrency, angle_groups.size());
                std::atomic<size_t> next_group{0};
                tbb::parallel_for(tbb::blocked_range<size_t>(0, worker_count, 1), [&](const tbb::blocked_range<size_t> &range) {
                    for (size_t worker = range.begin(); worker < range.end(); ++worker) {
                        (void) worker;
                        for (;;) {
                            const size_t group_index = next_group.fetch_add(1, std::memory_order_relaxed);
                            if (group_index >= angle_groups.size())
                                break;
                            safe_process_group(group_index);
                        }
                    }
                });
            } else {
                const auto probe = input.resource_probe ? input.resource_probe : sample_system_resources;
                auto sample_resources = [&]() -> std::optional<SystemResourceSample> {
                    try {
                        auto sample = probe();
                        if (sample && sample->total_memory_bytes > 0 &&
                            std::isfinite(sample->process_cpu_seconds))
                            return sample;
                    } catch (...) {
                    }
                    return std::nullopt;
                };

                const size_t cpu_limit = std::max<size_t>(1, std::min<size_t>(
                    angle_groups.size(), size_t(tbb::this_task_arena::max_concurrency())));
                uint64_t group_estimate = minimum_group_memory;
                size_t desired_groups = 1;
                size_t next_group = 0;
                std::atomic<size_t> active_groups{0};
                bool calibrated = false;
                bool waiting_for_memory = false;
                std::mutex completion_mutex;
                std::condition_variable completion_cv;
                tbb::task_group tasks;

                std::optional<SystemResourceSample> initial_sample = sample_resources();
                uint64_t calibration_baseline = initial_sample ? initial_sample->process_resident_bytes : 0;
                uint64_t calibration_peak = calibration_baseline;
                size_t peak_active_groups = 1;
                auto cpu_window_start = std::chrono::steady_clock::now();
                double cpu_seconds_start = initial_sample ? initial_sample->process_cpu_seconds : 0.;

                auto launch = [&](size_t group_index) {
                    active_groups.fetch_add(1, std::memory_order_relaxed);
                    try {
                        tasks.run([&, group_index]() {
                            try {
                                process_group(group_index);
                            } catch (const std::bad_alloc &) {
                                resource_exhausted.store(true, std::memory_order_relaxed);
                            }
                            active_groups.fetch_sub(1, std::memory_order_relaxed);
                            completion_cv.notify_one();
                        });
                    } catch (const std::bad_alloc &) {
                        active_groups.fetch_sub(1, std::memory_order_relaxed);
                        resource_exhausted.store(true, std::memory_order_relaxed);
                    }
                };

                while ((next_group < angle_groups.size() || active_groups.load(std::memory_order_relaxed) != 0) &&
                       !check_canceled() && !resource_exhausted.load(std::memory_order_relaxed)) {
                    const auto sample = sample_resources();
                    if (!initial_sample && sample && calibration_baseline == 0) {
                        calibration_baseline = sample->process_resident_bytes;
                        cpu_seconds_start = sample->process_cpu_seconds;
                    }
                    if (!calibrated && sample)
                        calibration_peak = std::max(calibration_peak, sample->process_resident_bytes);
                    if (calibrated && sample) {
                        peak_active_groups = std::max(peak_active_groups, active_groups.load(std::memory_order_relaxed));
                        const uint64_t increment = sample->process_resident_bytes > calibration_baseline ?
                            sample->process_resident_bytes - calibration_baseline : 0;
                        const uint64_t per_group = increment / peak_active_groups;
                        group_estimate = std::max(group_estimate,
                            per_group > UINT64_MAX / 2 ? UINT64_MAX : per_group * 2);
                    }

                    if (!calibrated && next_group > 0 && active_groups.load(std::memory_order_relaxed) == 0) {
                        const uint64_t observed = calibration_peak > calibration_baseline ? calibration_peak - calibration_baseline : 0;
                        group_estimate = std::max(minimum_group_memory,
                            observed > UINT64_MAX / 2 ? UINT64_MAX : observed * 2);
                        calibrated = true;
                        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - cpu_window_start).count();
                        const double used_cores = sample && elapsed > 0. ?
                            (sample->process_cpu_seconds - cpu_seconds_start) / elapsed : 0.;
                        if (sample && cpu_limit > 1 && (elapsed < 1. || used_cores < 0.7 * double(cpu_limit)))
                            desired_groups = 2;
                        BOOST_LOG_TRIVIAL(info) << "[DynaPin] automatic angle scheduling: estimate=" << group_estimate
                                                << " bytes, cpu_limit=" << cpu_limit << ", target=" << desired_groups;
                        cpu_window_start = std::chrono::steady_clock::now();
                        cpu_seconds_start = sample ? sample->process_cpu_seconds : 0.;
                    } else if (calibrated && sample && cpu_limit > desired_groups) {
                        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - cpu_window_start).count();
                        if (elapsed >= 1.) {
                            const double used_cores = (sample->process_cpu_seconds - cpu_seconds_start) / elapsed;
                            if (used_cores >= 0. && used_cores < 0.7 * double(cpu_limit)) {
                                ++desired_groups;
                                BOOST_LOG_TRIVIAL(info) << "[DynaPin] angle group target=" << desired_groups
                                                        << ", available=" << sample->available_memory_bytes
                                                        << " bytes, rss=" << sample->process_resident_bytes
                                                        << " bytes, estimate=" << group_estimate << " bytes";
                            }
                            cpu_window_start = std::chrono::steady_clock::now();
                            cpu_seconds_start = sample->process_cpu_seconds;
                        }
                    }

                    bool launched = false;
                    while (next_group < angle_groups.size() && !resource_exhausted.load(std::memory_order_relaxed) &&
                           active_groups.load(std::memory_order_relaxed) < desired_groups &&
                           (calibrated || active_groups.load(std::memory_order_relaxed) == 0) &&
                           (sample ? can_admit_angle_group(*sample, group_estimate, active_groups.load(std::memory_order_relaxed)) :
                                     active_groups.load(std::memory_order_relaxed) == 0)) {
                        if (check_canceled())
                            break;
                        if (waiting_for_memory) {
                            set_stage(phase_start == 5 ? PlacementProgressStage::CoarseSearch : PlacementProgressStage::LocalSearch);
                            waiting_for_memory = false;
                        }
                        launch(next_group++);
                        launched = true;
                    }
                    if (next_group == angle_groups.size() && active_groups.load(std::memory_order_relaxed) == 0)
                        break;
                    if (!launched && active_groups.load(std::memory_order_relaxed) == 0 && next_group < angle_groups.size() && sample &&
                        !can_admit_angle_group(*sample, group_estimate, 0) && !waiting_for_memory) {
                        set_stage(PlacementProgressStage::WaitingForMemory);
                        waiting_for_memory = true;
                        BOOST_LOG_TRIVIAL(info) << "[DynaPin] waiting for memory: available=" << sample->available_memory_bytes
                                                << " bytes, rss=" << sample->process_resident_bytes
                                                << " bytes, estimate=" << group_estimate << " bytes";
                    }
                    std::unique_lock<std::mutex> lock(completion_mutex);
                    completion_cv.wait_for(lock, std::chrono::milliseconds(100));
                }
                tasks.wait();
            }
        } else {
            auto process_candidate_range = [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    if (check_canceled())
                        return;
                    try {
                        values[i] = evaluator(candidates[i]);
                    } catch (const std::bad_alloc &) {
                        resource_exhausted.store(true, std::memory_order_relaxed);
                        return;
                    } catch (...) {
                    }
                    complete_candidate(i);
                }
            };

            if (!parallel || input.concurrency == 1) {
                process_candidate_range(0, candidates.size());
            } else {
                auto run_parallel = [&]() {
                    tbb::parallel_for(tbb::blocked_range<size_t>(0, candidates.size()),
                                      [&](const tbb::blocked_range<size_t> &range) {
                                          process_candidate_range(range.begin(), range.end());
                                      });
                };
                if (input.concurrency > 1) {
                    tbb::task_arena arena(static_cast<int>(input.concurrency));
                    arena.execute(run_parallel);
                } else {
                    run_parallel();
                }
            }
        }
        return values;
    };

    std::vector<PlacementCandidate> unique_coarse;
    unique_coarse.reserve(coarse.size());
    for (const PlacementCandidate &candidate : coarse) {
        const PlacementCandidate normalized_candidate{normalized_rotation_deg(candidate.rotation_deg), candidate.delta_y};
        if (evaluated.emplace(candidate_key(normalized_candidate)).second)
            unique_coarse.push_back(normalized_candidate);
    }
    set_stage(PlacementProgressStage::CoarseSearch);
    {
        const std::vector<std::optional<double>> values = evaluate_candidates(unique_coarse, 5, 75, has_rotation_ranges);
        if (resource_exhausted.load(std::memory_order_relaxed)) {
            result.status = PlacementStatus::ResourceExhausted;
            result.warning = "Insufficient memory for DynaPin placement search";
            set_progress(100);
            return result;
        }
        if (check_canceled()) {
            result.status = PlacementStatus::Canceled;
            set_progress(100);
            return result;
        }
        for (size_t i = 0; i < unique_coarse.size(); ++i)
            if (values[i] && finite(*values[i]) && *values[i] >= 0.)
                feasible.emplace_back(unique_coarse[i], *values[i]);
    }
    if (feasible.empty()) {
        result.status = PlacementStatus::NoFeasiblePose;
        result.warning = "No feasible DynaPin placement candidate";
        set_progress(100);
        return result;
    }

    std::sort(feasible.begin(), feasible.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.second != rhs.second)
            return lhs.second < rhs.second;
        return lhs.first.delta_y < rhs.first.delta_y;
    });
    const size_t seed_count = std::min<size_t>(5, feasible.size());
    std::set<CandidateKey> local_seen;
    std::vector<PlacementCandidate> unique_local;
    for (size_t seed_index = 0; seed_index < seed_count; ++seed_index) {
        for (const PlacementCandidate &candidate : local_candidates(feasible[seed_index].first, pitch_y)) {
            if (!local_seen.emplace(candidate_key(candidate)).second)
                continue;
            if (evaluated.emplace(candidate_key(candidate)).second)
                unique_local.push_back(candidate);
        }
    }
    set_stage(PlacementProgressStage::LocalSearch);
    {
        const std::vector<std::optional<double>> values = evaluate_candidates(unique_local, 75, 99, has_rotation_ranges);
        if (resource_exhausted.load(std::memory_order_relaxed)) {
            result.status = PlacementStatus::ResourceExhausted;
            result.warning = "Insufficient memory for DynaPin placement search";
            set_progress(100);
            return result;
        }
        if (check_canceled()) {
            result.status = PlacementStatus::Canceled;
            set_progress(100);
            return result;
        }
        for (size_t i = 0; i < unique_local.size(); ++i)
            if (values[i] && finite(*values[i]) && *values[i] >= 0.)
                feasible.emplace_back(unique_local[i], *values[i]);
    }

    // A cancellation can be raised by the evaluator itself, including on the
    // last candidate.  Check again before ranking so a canceled search never
    // leaks an otherwise valid winner to finalize().
    if (check_canceled()) {
        result.status = PlacementStatus::Canceled;
        set_progress(100);
        return result;
    }

    set_stage(PlacementProgressStage::Finalizing);
    double best_volume = std::numeric_limits<double>::infinity();
    for (const auto &entry : feasible)
        if (strictly_better(entry.second, best_volume))
            best_volume = entry.second;
    if (!finite(best_volume)) {
        result.status = PlacementStatus::NoFeasiblePose;
        result.warning = "No feasible DynaPin placement candidate";
        set_progress(100);
        return result;
    }

    std::optional<std::pair<PlacementCandidate, double>> winner;
    for (const auto &entry : feasible) {
        if (entry.second > best_volume + epsilon)
            continue;
        if (!winner || candidate_tie_less(entry.first, winner->first, input.current_rotation_deg))
            winner = entry;
    }
    if (!winner) {
        result.status = PlacementStatus::NoFeasiblePose;
        set_progress(100);
        return result;
    }

    result.candidate = winner->first;
    result.volume_mm3 = winner->second;
    if (winner->second < baseline_volume - epsilon) {
        result.status = PlacementStatus::Improved;
    } else {
        result.status = PlacementStatus::Unchanged;
        result.candidate = current;
        result.volume_mm3 = baseline_volume;
    }
    set_progress(100);
    return result;
}

} // namespace Slic3r::DynaPin
