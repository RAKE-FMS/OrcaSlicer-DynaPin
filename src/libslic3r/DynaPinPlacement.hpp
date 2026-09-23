#ifndef slic3r_DynaPinPlacement_hpp_
#define slic3r_DynaPinPlacement_hpp_

#include "DynaPin.hpp"
#include "ObjectID.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "Utils.hpp"

#include <Eigen/Geometry>

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
class Print;
class PrintObject;
class Model;
namespace DynaPin {

// A support area between two Z boundaries.  Polygons are in one common plate
// coordinate system and use the normal libslic3r scaling factor.
struct SupportSlab
{
    double   z_min = 0.;
    double   z_max = 0.;
    Polygons polygons;
};

// Computes the unioned support volume of all slabs.  Slabs may overlap in Z
// and XY; each XY region is counted once in every active Z interval.
double support_volume_mm3(const std::vector<SupportSlab> &slabs);
double support_volume_mm3(const Print &print);
std::vector<SupportSlab> generate_support_geometry(PrintObject &object);

struct PlacementCandidate
{
    double rotation_deg = 0.;
    double delta_y       = 0.;

    bool operator==(const PlacementCandidate &rhs) const
    {
        return rotation_deg == rhs.rotation_deg && delta_y == rhs.delta_y;
    }
};

struct DeltaYInterval
{
    double min = 0.;
    double max = 0.;
};

struct PlacementSceneSnapshot;
struct PlacementTask;
using DeltaYRangeProvider = std::function<std::optional<DeltaYInterval>(double rotation_deg)>;
using CandidateCompletedCallback = std::function<void(size_t candidate_index)>;
using AngleGroupEvaluator = std::function<void(double                                 rotation_deg,
                                               const std::vector<PlacementCandidate> &candidates,
                                               std::vector<std::optional<double>>    &results,
                                               const CandidateCompletedCallback     &completed)>;

// Return the continuous translation range that keeps a model bbox inside the
// printable Y range.  The caller may further narrow this range to the
// one-pitch search interval.
std::optional<DeltaYInterval> placement_delta_y_range_for_bbox(double bed_y_min,
                                                               double bed_y_max,
                                                               double bbox_min_y,
                                                               double bbox_max_y);
std::optional<DeltaYInterval> placement_delta_y_range_for_scene(const PlacementSceneSnapshot &scene,
                                                                double                        rotation_deg);

// Return an interval of up to one pitch.  If the bed clips only one side, the
// interval is shifted toward the available side while preserving its width.
std::optional<DeltaYInterval> placement_delta_y_interval(double lower, double upper, double pitch_y);

std::vector<PlacementCandidate> coarse_candidates(const DeltaYInterval &interval,
                                                   double               pitch_y,
                                                   double               current_rotation_deg = 0.,
                                                   double               current_delta_y = 0.);

std::vector<PlacementCandidate> local_candidates(const PlacementCandidate &seed, double pitch_y);

double normalized_rotation_deg(double rotation_deg);
double rotation_distance_deg(double lhs_deg, double rhs_deg);

// Apply a candidate around a fixed world-space center.  The initial matrix is
// used every time; candidates never accumulate rotation from a prior one.
Transform3d candidate_transform(const Transform3d &initial,
                                const Vec3d       &world_center,
                                double             rotation_deg,
                                double             delta_y);

enum class PlacementStatus {
    Improved,
    Unchanged,
    Canceled,
    InvalidConfig,
    NoFeasiblePose,
    ResourceExhausted,
};

// Pure state decisions used by the GUI reslice bridge.  Keeping these rules
// independent of Plater makes the one-shot optimization flow testable without
// constructing wxWidgets objects or a live worker.
enum class PlacementResliceAction {
    StartSearch,
    ContinueSlicing,
    WaitForWorker,
};

PlacementResliceAction placement_reslice_action(bool optimization_reslice_guard,
                                                bool slice_result_valid,
                                                bool force_placement_search,
                                                bool worker_idle);
bool placement_job_should_continue_reslice(PlacementStatus status, bool canceled, bool failed);

struct PlacementSearchInput
{
    double pitch_y = std::numeric_limits<double>::quiet_NaN();
    double y_min   = std::numeric_limits<double>::quiet_NaN();
    double y_max   = std::numeric_limits<double>::quiet_NaN();
    double current_rotation_deg = 0.;
    double current_delta_y = 0.;
    double current_volume_mm3 = std::numeric_limits<double>::quiet_NaN();
    // When provided, coarse search recomputes the feasible Y interval for
    // every rotation. The fixed y_min/y_max values remain the fallback used by
    // pure optimizer tests and callers without scene geometry.
    DeltaYRangeProvider delta_y_range_for_rotation;
    size_t              concurrency = 0;
    // Zero selects resource-aware scheduling; a positive value is a fixed
    // maximum number of concurrently cached angle groups.
    size_t              angle_group_concurrency = 0;
    std::function<std::optional<SystemResourceSample>()> resource_probe;
    AngleGroupEvaluator angle_evaluator;
};

// Conditions checked before a background placement search is started.  The
// GUI takes this snapshot on its thread; the worker only consumes the copied
// values and never reads live Plater state.
struct PlacementEligibility
{
    bool dynapin_enabled = false;
    bool placement_enabled = false;
    bool automatic_pin_selection = false;
    bool support_configuration_eligible = false;
    size_t selected_instance_count = 0;
    bool valid(std::string *error = nullptr) const;
};

struct PlacementResult
{
    PlacementStatus    status = PlacementStatus::InvalidConfig;
    PlacementCandidate candidate;
    double             volume_mm3 = std::numeric_limits<double>::quiet_NaN();
    size_t             evaluations = 0;
    std::string        warning;
};

struct PlacementTarget
{
    ObjectID object_id;
    ObjectID instance_id;
};

// Immutable input for a real placement evaluation.  The model and config are
// copied on the UI thread and owned by the snapshot; each candidate evaluation
// clones the model again before applying its transform, so the worker never
// touches the live Plater/Print objects.
struct PlacementSceneSnapshot
{
    std::shared_ptr<const Model> model;
    DynamicPrintConfig           config;
    ObjectID                     target_object_id;
    ObjectID                     target_instance_id;
    Transform3d                  initial_transform = Transform3d::Identity();
    Vec3d                        world_center = Vec3d::Zero();
    // The active plate is copied explicitly because a Plater may contain
    // several plate-local build volumes.  The model transforms are stored in
    // scene coordinates, while support slabs remain in the active plate's
    // local coordinate system.
    std::vector<Vec2d>            printable_area;
    double                        printable_height = std::numeric_limits<double>::quiet_NaN();
    Vec3d                         plate_origin = Vec3d::Zero();
    int                           plate_index = 0;
};

struct PlacementTask
{
    PlacementEligibility   eligibility;
    PlacementSearchInput   search;
    PlacementSceneSnapshot scene;
    bool                   tree_support = false;
};

struct PlacementPreparationResult
{
    std::optional<PlacementTask> task;
    std::string                  warning;
};

struct PlacementApplyResult
{
    bool        applied = false;
    std::string warning;
};

// The evaluator returns nullopt for a candidate rejected by bed or model
// constraints. It may throw; optimize_placement converts such a failure into
// NoFeasiblePose and preserves the live caller state.
using PlacementEvaluator = std::function<std::optional<double>(const PlacementCandidate &)>;
using CancelCallback = std::function<bool()>;
using ProgressCallback = std::function<void(int)>;
enum class PlacementProgressStage {
    EvaluatingCurrent,
    PreparingCandidates,
    CoarseSearch,
    LocalSearch,
    WaitingForMemory,
    Finalizing,
};
using ProgressStageCallback = std::function<void(PlacementProgressStage)>;

std::optional<double> evaluate_scene_candidate(const PlacementSceneSnapshot &scene,
                                               const PlacementCandidate     &candidate,
                                               const CancelCallback         &cancel = {});

void evaluate_scene_angle_group(const PlacementSceneSnapshot          &scene,
                                double                                 rotation_deg,
                                const std::vector<PlacementCandidate> &candidates,
                                std::vector<std::optional<double>>    &results,
                                const CandidateCompletedCallback      &completed = {},
                                const CancelCallback                  &cancel = {});

PlacementEvaluator  make_scene_evaluator(PlacementSceneSnapshot scene, const CancelCallback &cancel = {});
AngleGroupEvaluator make_scene_angle_evaluator(PlacementSceneSnapshot scene, const CancelCallback &cancel = {});

PlacementPreparationResult prepare_placement_task(const Model&                          model,
                                                  const Print&                          print,
                                                  const std::vector<Vec2d>&             printable_area,
                                                  double                                printable_height,
                                                  const Vec3d&                          plate_origin,
                                                  int                                   plate_index,
                                                  const std::optional<PlacementTarget>& preferred_target = {});

PlacementResult run_placement_task(PlacementTask                task,
                                   const CancelCallback&        cancel         = {},
                                   const ProgressCallback&      progress       = {},
                                   const ProgressStageCallback& progress_stage = {});

PlacementApplyResult apply_placement_result(Model&                       model,
                                            const PlacementTask&         task,
                                            const PlacementResult&       result,
                                            const std::function<void()>& before_apply = {});

PlacementResult optimize_placement(const PlacementSearchInput &input,
                                   const PlacementEvaluator   &evaluator,
                                   const CancelCallback        &cancel = {},
                                   const ProgressCallback      &progress = {},
                                   const ProgressStageCallback &progress_stage = {});

} // namespace DynaPin
} // namespace Slic3r

#endif // slic3r_DynaPinPlacement_hpp_
