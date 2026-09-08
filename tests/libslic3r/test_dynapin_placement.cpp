#include <catch2/catch_all.hpp>

#include "libslic3r/DynaPinPlacement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
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
    const DynaPin::PlacementResult result = DynaPin::optimize_placement(
        input,
        [&evaluated](const DynaPin::PlacementCandidate &candidate) {
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

TEST_CASE("DynaPin placement reslice flow consumes its one-shot guard", "[DynaPinPlacement]")
{
    CHECK(DynaPin::placement_reslice_action(false, true) == DynaPin::PlacementResliceAction::StartSearch);
    CHECK(DynaPin::placement_reslice_action(true, true) == DynaPin::PlacementResliceAction::ContinueSlicing);
    CHECK(DynaPin::placement_reslice_action(false, false) == DynaPin::PlacementResliceAction::WaitForWorker);

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

    eligibility.dynapin_enabled          = true;
    eligibility.automatic_pin_selection  = true;
    eligibility.normal_support           = true;
    eligibility.selected_instance_count  = 1;
    CHECK(eligibility.valid(&error));
}
