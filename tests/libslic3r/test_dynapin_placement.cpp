#include <catch2/catch_all.hpp>

#include "libslic3r/DynaPinPlacement.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Slic3r;

namespace {

Polygon square(double x0, double y0, double x1, double y1)
{
    return Polygon{{scaled(x0), scaled(y0)}, {scaled(x1), scaled(y0)}, {scaled(x1), scaled(y1)}, {scaled(x0), scaled(y1)}};
}

} // namespace

TEST_CASE("DynaPin placement support volume unions overlapping Z slabs", "[DynaPinPlacement]")
{
    const Polygon ten_by_ten = square(0., 0., 10., 10.);

    CHECK(DynaPin::support_volume_mm3({{0., 2., {ten_by_ten}}}) == Catch::Approx(200.));
    CHECK(DynaPin::support_volume_mm3({{0., 2., {ten_by_ten}}, {1., 3., {ten_by_ten}}}) == Catch::Approx(300.));
    CHECK(DynaPin::support_volume_mm3({}) == Catch::Approx(0.));

    Polygon hole = square(2., 2., 8., 8.);
    hole.make_clockwise();
    CHECK(DynaPin::support_volume_mm3({{0., 1., {ten_by_ten, hole}}}) == Catch::Approx(64.));
}

TEST_CASE("DynaPin placement support volume rejects invalid slabs", "[DynaPinPlacement]")
{
    const Polygon one_by_one = square(0., 0., 1., 1.);
    CHECK_THROWS_AS(DynaPin::support_volume_mm3({{std::numeric_limits<double>::quiet_NaN(), 1., {one_by_one}}}),
                    std::invalid_argument);
    CHECK_THROWS_AS(DynaPin::support_volume_mm3({{2., 1., {one_by_one}}}), std::invalid_argument);
    CHECK(DynaPin::support_volume_mm3({{1., 1., {one_by_one}}}) == Catch::Approx(0.));
}

TEST_CASE("DynaPin placement DeltaY interval keeps one pitch available", "[DynaPinPlacement]")
{
    for (const auto &[lower, upper, expected_lower, expected_upper] :
         std::vector<std::array<double, 4>>{{-100., 100., -8., 8.}, {-3., 100., -3., 13.}, {-100., 2., -14., 2.}, {-3., 2., -3., 2.}}) {
        const auto interval = DynaPin::placement_delta_y_interval(lower, upper, 16.);
        REQUIRE(interval);
        CHECK(interval->min == Catch::Approx(expected_lower));
        CHECK(interval->max == Catch::Approx(expected_upper));
    }
    CHECK(!DynaPin::placement_delta_y_interval(-1., 1., 0.));
    const auto negative_pitch = DynaPin::placement_delta_y_interval(-100., 100., -16.);
    REQUIRE(negative_pitch);
    CHECK(negative_pitch->min == Catch::Approx(-8.));
    CHECK(negative_pitch->max == Catch::Approx(8.));
}

TEST_CASE("DynaPin placement bbox Y range keeps both bbox edges inside the bed", "[DynaPinPlacement]")
{
    // A 20 mm high bbox at Y=[10,30] on a bed spanning [0,100] can move by
    // [-10,70].  Using the opposite bbox edges would incorrectly produce
    // [-30,90], which allows one edge to leave the bed near either boundary.
    const auto range = DynaPin::placement_delta_y_range_for_bbox(0., 100., 10., 30.);
    REQUIRE(range);
    CHECK(range->min == Catch::Approx(-10.));
    CHECK(range->max == Catch::Approx(70.));

    CHECK(!DynaPin::placement_delta_y_range_for_bbox(0., 10., 0., 20.));
}

TEST_CASE("DynaPin placement recomputes and shifts the Y interval for every coarse rotation", "[DynaPinPlacement]")
{
    DynaPin::PlacementSearchInput input;
    input.pitch_y            = 12.4;
    input.current_volume_mm3 = 1000.;
    input.delta_y_range_for_rotation = [](double rotation_deg) -> std::optional<DynaPin::DeltaYInterval> {
        if (rotation_deg == 0.)
            return DynaPin::DeltaYInterval{-3., 100.};
        if (rotation_deg == 5.)
            return DynaPin::DeltaYInterval{-100., 2.};
        return std::nullopt;
    };

    std::vector<DynaPin::PlacementCandidate> evaluated;
    std::mutex evaluated_mutex;
    const DynaPin::PlacementResult result = DynaPin::optimize_placement(
        input,
        [&evaluated, &evaluated_mutex](const DynaPin::PlacementCandidate &candidate) {
            std::lock_guard<std::mutex> lock(evaluated_mutex);
            evaluated.push_back(candidate);
            return std::optional<double>{1000.};
        });

    CHECK(result.status == DynaPin::PlacementStatus::Unchanged);
    CHECK(std::find(evaluated.begin(), evaluated.end(), DynaPin::PlacementCandidate{0., 9.4}) != evaluated.end());
    CHECK(std::find(evaluated.begin(), evaluated.end(), DynaPin::PlacementCandidate{5., -10.4}) != evaluated.end());
}

TEST_CASE("DynaPin placement candidates include current pose and deterministic endpoints", "[DynaPinPlacement]")
{
    const std::vector<DynaPin::PlacementCandidate> candidates = DynaPin::coarse_candidates({-8., 8.}, 16., 0., 0.);
    REQUIRE(!candidates.empty());
    CHECK(std::find(candidates.begin(), candidates.end(), DynaPin::PlacementCandidate{0., 0.}) != candidates.end());
    CHECK(std::find(candidates.begin(), candidates.end(), DynaPin::PlacementCandidate{0., -8.}) != candidates.end());
    CHECK(std::find(candidates.begin(), candidates.end(), DynaPin::PlacementCandidate{0., 8.}) != candidates.end());
    CHECK(DynaPin::local_candidates({355., 0.}, 16.).size() == 81);
    CHECK(DynaPin::normalized_rotation_deg(-1.) == Catch::Approx(359.));
    CHECK(DynaPin::rotation_distance_deg(359., 0.) == Catch::Approx(1.));
}

TEST_CASE("DynaPin placement candidates retain a shifted current pose", "[DynaPinPlacement]")
{
    const std::vector<DynaPin::PlacementCandidate> candidates = DynaPin::coarse_candidates({-8., 8.}, 16., 359., 20.);
    CHECK(std::find(candidates.begin(), candidates.end(), DynaPin::PlacementCandidate{359., 20.}) != candidates.end());
}

TEST_CASE("DynaPin placement reslice flow distinguishes explicit slicing from preview reuse", "[DynaPinPlacement]")
{
    CHECK(DynaPin::placement_reslice_action(false, false, false, true) == DynaPin::PlacementResliceAction::StartSearch);
    CHECK(DynaPin::placement_reslice_action(false, true, false, true) == DynaPin::PlacementResliceAction::ContinueSlicing);
    CHECK(DynaPin::placement_reslice_action(false, true, true, true) == DynaPin::PlacementResliceAction::StartSearch);
    CHECK(DynaPin::placement_reslice_action(true, false, true, true) == DynaPin::PlacementResliceAction::ContinueSlicing);
    CHECK(DynaPin::placement_reslice_action(false, false, false, false) == DynaPin::PlacementResliceAction::WaitForWorker);

    CHECK(DynaPin::placement_job_should_continue_reslice(DynaPin::PlacementStatus::Improved, false, false));
    CHECK(DynaPin::placement_job_should_continue_reslice(DynaPin::PlacementStatus::Unchanged, false, false));
    CHECK(DynaPin::placement_job_should_continue_reslice(DynaPin::PlacementStatus::NoFeasiblePose, false, false));
    CHECK_FALSE(DynaPin::placement_job_should_continue_reslice(DynaPin::PlacementStatus::Canceled, true, false));
    CHECK_FALSE(DynaPin::placement_job_should_continue_reslice(DynaPin::PlacementStatus::InvalidConfig, false, true));
}

TEST_CASE("DynaPin placement transform preserves world center while moving Y", "[DynaPinPlacement]")
{
    const Vec3d center{10., 20., 3.};
    const Transform3d transformed = DynaPin::candidate_transform(Transform3d::Identity(), center, 90., 4.);
    const Vec3d moved_center = transformed * center;
    CHECK(moved_center.x() == Catch::Approx(center.x()));
    CHECK(moved_center.y() == Catch::Approx(center.y() + 4.));
    CHECK(moved_center.z() == Catch::Approx(center.z()));
}

TEST_CASE("DynaPin placement result applies atomically to the original target", "[DynaPinPlacement]")
{
    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(10., 20., 5.));
    ModelInstance* instance = object->add_instance();
    instance->set_offset({30., 40., 0.});

    DynaPin::PlacementTask task;
    task.scene.target_object_id   = object->id();
    task.scene.target_instance_id = instance->id();
    task.scene.initial_transform  = instance->get_matrix();
    task.scene.world_center       = object->instance_bounding_box(*instance, false).center();

    DynaPin::PlacementResult improved;
    improved.status    = DynaPin::PlacementStatus::Improved;
    improved.candidate = {90., 4.};

    bool                                before_apply_called = false;
    const DynaPin::PlacementApplyResult applied = DynaPin::apply_placement_result(model, task, improved,
                                                                                  [&before_apply_called]() { before_apply_called = true; });
    CHECK(applied.applied);
    CHECK(before_apply_called);
    CHECK(instance->get_matrix().isApprox(DynaPin::candidate_transform(task.scene.initial_transform, task.scene.world_center, 90., 4.),
                                          1e-7));

    SECTION("a stale initial transform is rejected without another change")
    {
        const Transform3d                   already_applied = instance->get_matrix();
        const DynaPin::PlacementApplyResult stale           = DynaPin::apply_placement_result(model, task, improved);
        CHECK_FALSE(stale.applied);
        CHECK(instance->get_matrix().isApprox(already_applied, 1e-7));
    }
}

TEST_CASE("DynaPin placement result leaves the model unchanged when no improvement exists", "[DynaPinPlacement]")
{
    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(10., 20., 5.));
    ModelInstance* instance = object->add_instance();

    DynaPin::PlacementTask task;
    task.scene.target_object_id   = object->id();
    task.scene.target_instance_id = instance->id();
    task.scene.initial_transform  = instance->get_matrix();

    DynaPin::PlacementResult unchanged;
    unchanged.status                            = DynaPin::PlacementStatus::Unchanged;
    const Transform3d                   before  = instance->get_matrix();
    const DynaPin::PlacementApplyResult applied = DynaPin::apply_placement_result(model, task, unchanged);
    CHECK_FALSE(applied.applied);
    CHECK(instance->get_matrix().isApprox(before, 1e-7));
}

TEST_CASE("DynaPin placement applies only significant improvements", "[DynaPinPlacement]")
{
    DynaPin::PlacementSearchInput input;
    input.pitch_y            = 16.;
    input.y_min              = -8.;
    input.y_max              = 8.;
    input.current_volume_mm3 = 1000.;

    SECTION("epsilon-sized decrease remains unchanged")
    {
        const DynaPin::PlacementResult result = DynaPin::optimize_placement(input, [](const DynaPin::PlacementCandidate &) {
            return std::optional<double>{999.};
        });
        CHECK(result.status == DynaPin::PlacementStatus::Unchanged);
        CHECK(result.candidate == DynaPin::PlacementCandidate{0., 0.});
    }

    SECTION("larger decrease is applied")
    {
        const DynaPin::PlacementResult result = DynaPin::optimize_placement(input, [](const DynaPin::PlacementCandidate &) {
            return std::optional<double>{998.9};
        });
        CHECK(result.status == DynaPin::PlacementStatus::Improved);
        CHECK(result.volume_mm3 == Catch::Approx(998.9));
    }

    SECTION("duplicate coarse and local poses are evaluated once")
    {
        std::vector<DynaPin::PlacementCandidate> evaluated;
        const DynaPin::PlacementResult result = DynaPin::optimize_placement(
            input,
            [&evaluated](const DynaPin::PlacementCandidate &candidate) {
                evaluated.push_back(candidate);
                return std::optional<double>{1000.};
            });
        CHECK(result.status == DynaPin::PlacementStatus::Unchanged);
        std::sort(evaluated.begin(), evaluated.end(), [](const auto &lhs, const auto &rhs) {
            if (lhs.rotation_deg != rhs.rotation_deg)
                return lhs.rotation_deg < rhs.rotation_deg;
            return lhs.delta_y < rhs.delta_y;
        });
        CHECK(std::adjacent_find(evaluated.begin(), evaluated.end()) == evaluated.end());
        CHECK(result.evaluations == evaluated.size());
    }

    SECTION("cancellation does not return a winner")
    {
        bool canceled = false;
        const DynaPin::PlacementResult result = DynaPin::optimize_placement(
            input,
            [](const DynaPin::PlacementCandidate &) { return std::optional<double>{998.}; },
            [&canceled]() {
                const bool was_canceled = canceled;
                canceled = true;
                return was_canceled;
            });
        CHECK(result.status == DynaPin::PlacementStatus::Canceled);
    }

    SECTION("cancellation raised by the last evaluation is still observed")
    {
        bool canceled = false;
        const DynaPin::PlacementResult result = DynaPin::optimize_placement(
            input,
            [&canceled](const DynaPin::PlacementCandidate &) {
                canceled = true;
                return std::optional<double>{998.};
            },
            [&canceled]() { return canceled; });
        CHECK(result.status == DynaPin::PlacementStatus::Canceled);
    }

    SECTION("cancellation during the current pose is not reported as infeasible")
    {
        bool canceled = false;
        const DynaPin::PlacementResult result = DynaPin::optimize_placement(
            input,
            [&canceled](const DynaPin::PlacementCandidate &) {
                canceled = true;
                return std::optional<double>{};
            },
            [&canceled]() { return canceled; });
        CHECK(result.status == DynaPin::PlacementStatus::Canceled);
    }
}

TEST_CASE("DynaPin placement reports infeasible candidates", "[DynaPinPlacement]")
{
    DynaPin::PlacementSearchInput input;
    input.pitch_y = 16.;
    input.y_min   = -8.;
    input.y_max   = 8.;

    const DynaPin::PlacementResult result = DynaPin::optimize_placement(input, [](const DynaPin::PlacementCandidate &) {
        return std::optional<double>{};
    });
    CHECK(result.status == DynaPin::PlacementStatus::NoFeasiblePose);
}

TEST_CASE("DynaPin placement eligibility is a complete snapshot gate", "[DynaPinPlacement]")
{
    DynaPin::PlacementEligibility eligibility;
    std::string error;
    CHECK(!eligibility.valid(&error));
    CHECK(error == "DynaPin support optimization is disabled");

    eligibility.dynapin_enabled = true;
    CHECK(!eligibility.valid(&error));
    CHECK(error == "DynaPin position/rotation optimization is disabled");

    eligibility.placement_enabled        = true;
    eligibility.automatic_pin_selection  = true;
    eligibility.normal_support           = true;
    eligibility.selected_instance_count  = 1;
    CHECK(eligibility.valid(&error));
}

TEST_CASE("DynaPin placement angle evaluator matches flat evaluator deterministically", "[DynaPinPlacement]")
{
    auto objective_func = [](const DynaPin::PlacementCandidate &c) -> std::optional<double> {
        const double rot_diff = std::abs(DynaPin::rotation_distance_deg(c.rotation_deg, 45.));
        const double y_diff   = std::abs(c.delta_y - 3.2);
        return 500. + rot_diff * 2. + y_diff * 5.;
    };

    DynaPin::PlacementSearchInput input_base;
    input_base.pitch_y            = 16.;
    input_base.y_min              = -8.;
    input_base.y_max              = 8.;
    input_base.current_volume_mm3 = 1000.;
    input_base.delta_y_range_for_rotation = [](double rot) -> std::optional<DynaPin::DeltaYInterval> {
        return DynaPin::DeltaYInterval{-8., 8.};
    };

    // 1. Flat parallel evaluation
    DynaPin::PlacementSearchInput input_flat = input_base;
    input_flat.concurrency                   = 0;
    const DynaPin::PlacementResult result_flat = DynaPin::optimize_placement(input_flat, objective_func);

    // 2. Grouped serial evaluation (concurrency = 1)
    DynaPin::PlacementSearchInput input_grouped_serial = input_base;
    input_grouped_serial.concurrency                   = 1;
    input_grouped_serial.angle_group_concurrency       = 1;
    input_grouped_serial.angle_evaluator = [&](double rot, const std::vector<DynaPin::PlacementCandidate> &cands,
                                               std::vector<std::optional<double>> &res,
                                               const DynaPin::CandidateCompletedCallback &completed) {
        res.resize(cands.size());
        for (size_t i = 0; i < cands.size(); ++i) {
            res[i] = objective_func(cands[i]);
            completed(i);
        }
    };
    const DynaPin::PlacementResult result_grouped_serial = DynaPin::optimize_placement(input_grouped_serial, objective_func);

    // 3. Grouped parallel evaluation (concurrency = 0)
    DynaPin::PlacementSearchInput input_grouped_parallel = input_grouped_serial;
    input_grouped_parallel.concurrency                   = 0;
    input_grouped_parallel.angle_group_concurrency       = 0;
    const DynaPin::PlacementResult result_grouped_parallel =
        DynaPin::optimize_placement(input_grouped_parallel, objective_func);

    CHECK(result_flat.status == DynaPin::PlacementStatus::Improved);
    CHECK(result_grouped_serial.status == result_flat.status);
    CHECK(result_grouped_parallel.status == result_flat.status);

    CHECK(result_grouped_serial.candidate.rotation_deg == Catch::Approx(result_flat.candidate.rotation_deg));
    CHECK(result_grouped_serial.candidate.delta_y == Catch::Approx(result_flat.candidate.delta_y));
    CHECK(result_grouped_serial.volume_mm3 == Catch::Approx(result_flat.volume_mm3));

    CHECK(result_grouped_parallel.candidate.rotation_deg == Catch::Approx(result_flat.candidate.rotation_deg));
    CHECK(result_grouped_parallel.candidate.delta_y == Catch::Approx(result_flat.candidate.delta_y));
    CHECK(result_grouped_parallel.volume_mm3 == Catch::Approx(result_flat.volume_mm3));
}

TEST_CASE("DynaPin placement reports progress before an angle batch completes", "[DynaPinPlacement]")
{
    DynaPin::PlacementSearchInput input;
    input.pitch_y                 = 16.;
    input.current_volume_mm3      = 1000.;
    input.angle_group_concurrency = 1;
    input.delta_y_range_for_rotation = [](double rotation_deg) -> std::optional<DynaPin::DeltaYInterval> {
        return rotation_deg == 0. ? std::optional<DynaPin::DeltaYInterval>{{-8., 8.}} : std::nullopt;
    };

    std::mutex              gate_mutex;
    std::condition_variable gate_cv;
    bool                    first_candidate_completed = false;
    bool                    release_batch             = false;
    input.angle_evaluator = [&](double, const std::vector<DynaPin::PlacementCandidate> &candidates,
                                std::vector<std::optional<double>> &results,
                                const DynaPin::CandidateCompletedCallback &completed) {
        results.assign(candidates.size(), 800.);
        for (size_t i = 0; i < candidates.size(); ++i) {
            completed(i);
            if (i == 0) {
                std::unique_lock<std::mutex> lock(gate_mutex);
                first_candidate_completed = true;
                gate_cv.notify_one();
                gate_cv.wait(lock, [&release_batch]() { return release_batch; });
            }
        }
    };

    std::mutex       progress_mutex;
    std::vector<int> progress_values;
    std::vector<DynaPin::PlacementProgressStage> progress_stages;
    auto result = std::async(std::launch::async, [&]() {
        return DynaPin::optimize_placement(
            input,
            [](const DynaPin::PlacementCandidate &) { return std::optional<double>{1000.}; },
            {},
            [&progress_mutex, &progress_values](int progress) {
                std::lock_guard<std::mutex> lock(progress_mutex);
                progress_values.push_back(progress);
            },
            [&progress_mutex, &progress_stages](DynaPin::PlacementProgressStage stage) {
                std::lock_guard<std::mutex> lock(progress_mutex);
                progress_stages.push_back(stage);
            });
    });

    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(gate_cv.wait_for(lock, std::chrono::seconds(5), [&first_candidate_completed]() {
            return first_candidate_completed;
        }));
    }
    {
        std::lock_guard<std::mutex> lock(progress_mutex);
        CHECK(std::any_of(progress_values.begin(), progress_values.end(), [](int progress) {
            return progress > 5 && progress < 75;
        }));
    }
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_batch = true;
    }
    gate_cv.notify_one();

    CHECK(result.get().status == DynaPin::PlacementStatus::Improved);
    {
        std::lock_guard<std::mutex> lock(progress_mutex);
        CHECK(std::is_sorted(progress_values.begin(), progress_values.end()));
        CHECK(std::adjacent_find(progress_values.begin(), progress_values.end()) == progress_values.end());
        REQUIRE(progress_stages.size() == 5);
        CHECK(progress_stages == std::vector<DynaPin::PlacementProgressStage>{
            DynaPin::PlacementProgressStage::EvaluatingCurrent,
            DynaPin::PlacementProgressStage::PreparingCandidates,
            DynaPin::PlacementProgressStage::CoarseSearch,
            DynaPin::PlacementProgressStage::LocalSearch,
            DynaPin::PlacementProgressStage::Finalizing,
        });
    }
}

TEST_CASE("DynaPin placement limits concurrent angle groups without concurrent progress callbacks", "[DynaPinPlacement]")
{
    DynaPin::PlacementSearchInput input;
    input.pitch_y                 = 16.;
    input.current_volume_mm3      = 1000.;
    input.angle_group_concurrency = 2;
    input.delta_y_range_for_rotation = [](double) -> std::optional<DynaPin::DeltaYInterval> {
        return DynaPin::DeltaYInterval{0., 0.};
    };

    std::atomic<int> active_groups{0};
    std::atomic<int> peak_groups{0};
    std::mutex              group_mutex;
    std::condition_variable group_cv;
    int                     initial_groups_started = 0;
    input.angle_evaluator = [&](double, const std::vector<DynaPin::PlacementCandidate> &candidates,
                                std::vector<std::optional<double>> &results,
                                const DynaPin::CandidateCompletedCallback &completed) {
        const int active = active_groups.fetch_add(1) + 1;
        int       peak   = peak_groups.load();
        while (peak < active && !peak_groups.compare_exchange_weak(peak, active)) {}
        {
            std::unique_lock<std::mutex> lock(group_mutex);
            if (initial_groups_started < 2) {
                ++initial_groups_started;
                group_cv.notify_all();
                group_cv.wait_for(lock, std::chrono::seconds(5), [&initial_groups_started]() {
                    return initial_groups_started >= 2;
                });
            }
        }
        results.assign(candidates.size(), 800.);
        for (size_t i = 0; i < candidates.size(); ++i)
            completed(i);
        active_groups.fetch_sub(1);
    };

    std::atomic<int> active_progress_callbacks{0};
    std::atomic<int> peak_progress_callbacks{0};
    const DynaPin::PlacementResult result = DynaPin::optimize_placement(
        input,
        [](const DynaPin::PlacementCandidate &) { return std::optional<double>{1000.}; },
        {},
        [&](int) {
            const int active = active_progress_callbacks.fetch_add(1) + 1;
            int       peak   = peak_progress_callbacks.load();
            while (peak < active && !peak_progress_callbacks.compare_exchange_weak(peak, active)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            active_progress_callbacks.fetch_sub(1);
        });

    CHECK(result.status == DynaPin::PlacementStatus::Improved);
    CHECK(peak_groups.load() == 2);
    CHECK(peak_progress_callbacks.load() == 1);
}

TEST_CASE("DynaPin placement angle evaluator supports cancellation", "[DynaPinPlacement]")
{
    DynaPin::PlacementSearchInput input;
    input.pitch_y            = 16.;
    input.y_min              = -8.;
    input.y_max              = 8.;
    input.current_volume_mm3 = 1000.;
    input.delta_y_range_for_rotation = [](double rot) -> std::optional<DynaPin::DeltaYInterval> {
        return DynaPin::DeltaYInterval{-8., 8.};
    };

    std::atomic<bool> canceled{false};
    input.angle_evaluator = [&](double rot, const std::vector<DynaPin::PlacementCandidate> &cands,
                                std::vector<std::optional<double>> &res,
                                const DynaPin::CandidateCompletedCallback &completed) {
        canceled.store(true);
        res.assign(cands.size(), 800.);
    };

    const DynaPin::PlacementResult result = DynaPin::optimize_placement(
        input,
        [](const DynaPin::PlacementCandidate &) { return std::optional<double>{800.}; },
        [&canceled]() { return canceled.load(); });

    CHECK(result.status == DynaPin::PlacementStatus::Canceled);
    CHECK(result.evaluations == 1);
}

TEST_CASE("DynaPin placement auto scheduling waits for memory and resumes", "[DynaPinPlacement]")
{
    DynaPin::PlacementSearchInput input;
    input.pitch_y = 16.;
    input.current_volume_mm3 = 1000.;
    input.angle_group_concurrency = 0;
    input.delta_y_range_for_rotation = [](double) -> std::optional<DynaPin::DeltaYInterval> {
        return DynaPin::DeltaYInterval{0., 0.};
    };
    std::atomic<bool> memory_available{false};
    std::atomic<int> evaluated_groups{0};
    input.resource_probe = [&]() -> std::optional<SystemResourceSample> {
        return SystemResourceSample{16ull << 30, memory_available.load() ? 8ull << 30 : 1ull << 30,
                                    1ull << 30, 0.};
    };
    input.angle_evaluator = [&](double, const std::vector<DynaPin::PlacementCandidate> &candidates,
                                std::vector<std::optional<double>> &results,
                                const DynaPin::CandidateCompletedCallback &completed) {
        ++evaluated_groups;
        results.assign(candidates.size(), 800.);
        for (size_t i = 0; i < candidates.size(); ++i)
            completed(i);
    };
    std::mutex stage_mutex;
    std::condition_variable stage_cv;
    bool waiting = false;
    bool resumed = false;
    auto result = std::async(std::launch::async, [&]() {
        return DynaPin::optimize_placement(input, [](const DynaPin::PlacementCandidate &) {
            return std::optional<double>{1000.};
        }, {}, {}, [&](DynaPin::PlacementProgressStage stage) {
            std::lock_guard<std::mutex> lock(stage_mutex);
            if (stage == DynaPin::PlacementProgressStage::WaitingForMemory)
                waiting = true;
            if (waiting && (stage == DynaPin::PlacementProgressStage::CoarseSearch ||
                            stage == DynaPin::PlacementProgressStage::LocalSearch))
                resumed = true;
            stage_cv.notify_all();
        });
    });
    {
        std::unique_lock<std::mutex> lock(stage_mutex);
        REQUIRE(stage_cv.wait_for(lock, std::chrono::seconds(5), [&]() { return waiting; }));
    }
    CHECK(evaluated_groups.load() == 0);
    memory_available = true;
    {
        std::unique_lock<std::mutex> lock(stage_mutex);
        REQUIRE(stage_cv.wait_for(lock, std::chrono::seconds(5), [&]() { return resumed; }));
    }
    CHECK(result.get().status == DynaPin::PlacementStatus::Improved);
    CHECK(evaluated_groups.load() > 0);
}

TEST_CASE("DynaPin placement auto scheduling can cancel while waiting for memory", "[DynaPinPlacement]")
{
    DynaPin::PlacementSearchInput input;
    input.pitch_y = 16.;
    input.current_volume_mm3 = 1000.;
    input.delta_y_range_for_rotation = [](double) -> std::optional<DynaPin::DeltaYInterval> {
        return DynaPin::DeltaYInterval{0., 0.};
    };
    input.resource_probe = []() -> std::optional<SystemResourceSample> {
        return SystemResourceSample{16ull << 30, 1ull << 30, 1ull << 30, 0.};
    };
    input.angle_evaluator = [](double, const std::vector<DynaPin::PlacementCandidate> &candidates,
                               std::vector<std::optional<double>> &results,
                               const DynaPin::CandidateCompletedCallback &completed) {
        results.assign(candidates.size(), 800.);
        for (size_t i = 0; i < candidates.size(); ++i)
            completed(i);
    };
    std::atomic<bool> canceled{false};
    std::mutex stage_mutex;
    std::condition_variable stage_cv;
    bool waiting = false;
    auto result = std::async(std::launch::async, [&]() {
        return DynaPin::optimize_placement(input, [](const DynaPin::PlacementCandidate &) {
            return std::optional<double>{800.};
        }, [&]() { return canceled.load(); }, {}, [&](DynaPin::PlacementProgressStage stage) {
            if (stage == DynaPin::PlacementProgressStage::WaitingForMemory) {
                std::lock_guard<std::mutex> lock(stage_mutex);
                waiting = true;
                stage_cv.notify_one();
            }
        });
    });
    {
        std::unique_lock<std::mutex> lock(stage_mutex);
        REQUIRE(stage_cv.wait_for(lock, std::chrono::seconds(5), [&]() { return waiting; }));
    }
    canceled = true;
    CHECK(result.get().status == DynaPin::PlacementStatus::Canceled);
}

TEST_CASE("DynaPin placement auto scheduling falls back to one group when resource sampling fails", "[DynaPinPlacement]")
{
    DynaPin::PlacementSearchInput input;
    input.pitch_y = 16.;
    input.current_volume_mm3 = 1000.;
    input.delta_y_range_for_rotation = [](double) -> std::optional<DynaPin::DeltaYInterval> {
        return DynaPin::DeltaYInterval{0., 0.};
    };
    input.resource_probe = []() -> std::optional<SystemResourceSample> { return std::nullopt; };
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    input.angle_evaluator = [&](double, const std::vector<DynaPin::PlacementCandidate> &candidates,
                                std::vector<std::optional<double>> &results,
                                const DynaPin::CandidateCompletedCallback &completed) {
        const int now = ++active;
        peak = std::max(peak.load(), now);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        results.assign(candidates.size(), 800.);
        for (size_t i = 0; i < candidates.size(); ++i)
            completed(i);
        --active;
    };
    const auto result = DynaPin::optimize_placement(input, [](const DynaPin::PlacementCandidate &) {
        return std::optional<double>{1000.};
    });
    CHECK(result.status == DynaPin::PlacementStatus::Improved);
    CHECK(peak.load() == 1);
}

TEST_CASE("DynaPin placement reports resource exhaustion from angle evaluation", "[DynaPinPlacement]")
{
    DynaPin::PlacementSearchInput input;
    input.pitch_y = 16.;
    input.current_volume_mm3 = 1000.;
    input.angle_group_concurrency = 1;
    input.delta_y_range_for_rotation = [](double) -> std::optional<DynaPin::DeltaYInterval> {
        return DynaPin::DeltaYInterval{0., 0.};
    };
    input.angle_evaluator = [](double, const std::vector<DynaPin::PlacementCandidate> &,
                               std::vector<std::optional<double>> &,
                               const DynaPin::CandidateCompletedCallback &) {
        throw std::bad_alloc();
    };
    const auto result = DynaPin::optimize_placement(input, [](const DynaPin::PlacementCandidate &) {
        return std::optional<double>{1000.};
    });
    CHECK(result.status == DynaPin::PlacementStatus::ResourceExhausted);
    CHECK(!DynaPin::placement_job_should_continue_reslice(result.status, false, false));
}

TEST_CASE("DynaPin placement auto scheduling respects memory admission", "[DynaPinPlacement]")
{
    DynaPin::PlacementSearchInput input;
    input.pitch_y = 16.;
    input.current_volume_mm3 = 1000.;
    input.delta_y_range_for_rotation = [](double) -> std::optional<DynaPin::DeltaYInterval> {
        return DynaPin::DeltaYInterval{0., 0.};
    };
    input.resource_probe = []() -> std::optional<SystemResourceSample> {
        return SystemResourceSample{16ull << 30, 43ull * (1ull << 30) / 10, 1ull << 30, 0.};
    };
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> groups{0};
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    int waiting_groups = 0;
    input.angle_evaluator = [&](double, const std::vector<DynaPin::PlacementCandidate> &candidates,
                                std::vector<std::optional<double>> &results,
                                const DynaPin::CandidateCompletedCallback &completed) {
        const int group = ++groups;
        const int now = ++active;
        int old_peak = peak.load();
        while (old_peak < now && !peak.compare_exchange_weak(old_peak, now)) {}
        if (group == 2 || group == 3) {
            std::unique_lock<std::mutex> lock(gate_mutex);
            ++waiting_groups;
            gate_cv.notify_all();
            gate_cv.wait_for(lock, std::chrono::seconds(5), [&]() { return waiting_groups >= 2; });
        }
        results.assign(candidates.size(), 800.);
        for (size_t i = 0; i < candidates.size(); ++i)
            completed(i);
        --active;
    };
    const auto result = DynaPin::optimize_placement(input, [](const DynaPin::PlacementCandidate &) {
        return std::optional<double>{1000.};
    });
    CHECK(result.status == DynaPin::PlacementStatus::Improved);
    CHECK(waiting_groups == 2);
    CHECK(peak.load() == 2);
}
