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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace Slic3r::DynaPin {
namespace {

constexpr double angle_step_deg = 5.;
constexpr double tie_angle_epsilon = 1e-9;
constexpr double coordinate_epsilon = 1e-7;

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
    if (!normal_support)
        return fail("DynaPin placement optimization requires Normal support");
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
    return !canceled && !failed && status != PlacementStatus::Canceled;
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
    // This call runs update_dynapin_selection() before support generation.
    // The existing blocker collision scan therefore removes only colliding
    // pins while the candidate pose remains eligible for volume scoring.
    if (!candidate_print.generate_normal_support_geometry_only(slabs, cancel))
        return std::nullopt;
    if (cancel && cancel())
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

std::vector<SupportSlab> generate_normal_support_geometry(PrintObject &object)
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

    for (int angle = 0; angle < 360; angle += static_cast<int>(angle_step_deg)) {
        const std::optional<DeltaYInterval> feasible = input.delta_y_range_for_rotation(double(angle));
        if (!feasible)
            continue;
        const std::optional<DeltaYInterval> interval = placement_delta_y_interval(feasible->min, feasible->max, pitch_y);
        if (!interval)
            continue;

        for (int i = 0;; ++i) {
            const double delta_y = interval->min + double(i) * step;
            if (delta_y > interval->max + coordinate_epsilon)
                break;
            append_unique(candidates, seen, {double(angle), delta_y});
        }
        append_unique(candidates, seen, {double(angle), interval->min});
        append_unique(candidates, seen, {double(angle), interval->max});
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

    Model          candidate_model(*scene.model);
    ModelObject   *target_object   = nullptr;
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

    target_instance->set_transformation(Geometry::Transformation(candidate_transform(
        scene.initial_transform, scene.world_center, rotation_deg, 0.)));
    target_object->invalidate_bounding_box();
    const BoundingBoxf3 bbox = target_object->instance_bounding_box(*target_instance, false);
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
                                   const ProgressCallback      &progress)
{
    PlacementResult result;
    auto set_progress = [&progress](int value) {
        if (progress)
            progress(std::max(0, std::min(100, value)));
    };
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

    const double baseline_volume = finite(input.current_volume_mm3) && input.current_volume_mm3 >= 0. ? input.current_volume_mm3 : *current_volume;
    const double epsilon = std::max(1., baseline_volume * 0.001);
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
    const size_t estimated_total = std::max<size_t>(1, 1 + coarse.size() + 5 * 81);
    size_t completed = 0;
    std::vector<std::pair<PlacementCandidate, double>> feasible;
    feasible.reserve(coarse.size());
    std::set<CandidateKey> evaluated;
    evaluated.emplace(candidate_key(current));
    feasible.emplace_back(current, *current_volume);

    std::atomic<bool> canceled{false};
    auto check_canceled = [&]() {
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

    auto evaluate_parallel = [&](const std::vector<PlacementCandidate> &candidates) {
        std::vector<std::optional<double>> values(candidates.size());
        std::atomic<size_t> evaluations{0};
        tbb::parallel_for(tbb::blocked_range<size_t>(0, candidates.size()), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                if (check_canceled())
                    return;
                try {
                    values[i] = evaluator(candidates[i]);
                    evaluations.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    evaluations.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        const size_t evaluated_count = evaluations.load(std::memory_order_relaxed);
        result.evaluations += evaluated_count;
        completed += evaluated_count;
        set_progress(static_cast<int>(100. * double(completed) / double(estimated_total)));
        return values;
    };

    auto evaluate = [&](const PlacementCandidate &candidate) -> std::optional<double> {
        if (cancel && cancel())
            return std::nullopt;
        const PlacementCandidate normalized_candidate{normalized_rotation_deg(candidate.rotation_deg), candidate.delta_y};
        if (!evaluated.emplace(candidate_key(normalized_candidate)).second)
            return std::nullopt;
        try {
            std::optional<double> value = evaluator(normalized_candidate);
            ++result.evaluations;
            ++completed;
            set_progress(static_cast<int>(100. * double(completed) / double(estimated_total)));
            if (value && finite(*value) && *value >= 0.)
                return value;
        } catch (...) {
            ++result.evaluations;
            ++completed;
            set_progress(static_cast<int>(100. * double(completed) / double(estimated_total)));
        }
        return std::nullopt;
    };

    if (has_rotation_ranges) {
        std::vector<PlacementCandidate> unique_coarse;
        unique_coarse.reserve(coarse.size());
        for (const PlacementCandidate &candidate : coarse) {
            const PlacementCandidate normalized_candidate{normalized_rotation_deg(candidate.rotation_deg), candidate.delta_y};
            if (evaluated.emplace(candidate_key(normalized_candidate)).second)
                unique_coarse.push_back(normalized_candidate);
        }
        const std::vector<std::optional<double>> values = evaluate_parallel(unique_coarse);
        if (check_canceled()) {
            result.status = PlacementStatus::Canceled;
            set_progress(100);
            return result;
        }
        for (size_t i = 0; i < unique_coarse.size(); ++i)
            if (values[i] && finite(*values[i]) && *values[i] >= 0.)
                feasible.emplace_back(unique_coarse[i], *values[i]);
    } else {
        for (const PlacementCandidate &candidate : coarse) {
            if (cancel && cancel()) {
                result.status = PlacementStatus::Canceled;
                set_progress(100);
                return result;
            }
            if (const std::optional<double> value = evaluate(candidate))
                feasible.emplace_back(candidate, *value);
        }
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
    if (has_rotation_ranges) {
        std::vector<PlacementCandidate> unique_local;
        for (size_t seed_index = 0; seed_index < seed_count; ++seed_index) {
            for (const PlacementCandidate &candidate : local_candidates(feasible[seed_index].first, pitch_y)) {
                if (!local_seen.emplace(candidate_key(candidate)).second)
                    continue;
                if (evaluated.emplace(candidate_key(candidate)).second)
                    unique_local.push_back(candidate);
            }
        }
        const std::vector<std::optional<double>> values = evaluate_parallel(unique_local);
        if (check_canceled()) {
            result.status = PlacementStatus::Canceled;
            set_progress(100);
            return result;
        }
        for (size_t i = 0; i < unique_local.size(); ++i)
            if (values[i] && finite(*values[i]) && *values[i] >= 0.)
                feasible.emplace_back(unique_local[i], *values[i]);
    } else {
        for (size_t seed_index = 0; seed_index < seed_count; ++seed_index) {
            for (const PlacementCandidate &candidate : local_candidates(feasible[seed_index].first, pitch_y)) {
                if (!local_seen.emplace(candidate_key(candidate)).second)
                    continue;
                if (cancel && cancel()) {
                    result.status = PlacementStatus::Canceled;
                    set_progress(100);
                    return result;
                }
                if (const std::optional<double> value = evaluate(candidate))
                    feasible.emplace_back(candidate, *value);
            }
        }
    }

    // A cancellation can be raised by the evaluator itself, including on the
    // last candidate.  Check again before ranking so a canceled search never
    // leaks an otherwise valid winner to finalize().
    if (check_canceled()) {
        result.status = PlacementStatus::Canceled;
        set_progress(100);
        return result;
    }

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
