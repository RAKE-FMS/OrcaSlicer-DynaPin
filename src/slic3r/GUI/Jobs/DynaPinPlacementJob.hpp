#ifndef ORCASLICER_DYNAPIN_PLACEMENT_JOB_HPP
#define ORCASLICER_DYNAPIN_PLACEMENT_JOB_HPP

#include "Job.hpp"

#include "libslic3r/DynaPinPlacement.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace Slic3r::GUI {

// Everything needed by the worker is copied before it starts.  In
// particular, evaluator must own a temporary Model/Print and must never
// capture Plater or another live UI object.
struct DynaPinPlacementSnapshot
{
    DynaPin::PlacementEligibility eligibility;
    DynaPin::PlacementSearchInput search;
    DynaPin::PlacementSceneSnapshot scene;
    DynaPin::PlacementDetailedEvaluator evaluator;
    ObjectID target_instance_id;
    std::uint64_t input_generation = 0;
};

class DynaPinPlacementJob final : public Job
{
public:
    using ApplyCallback = std::function<bool(const DynaPinPlacementSnapshot &, const DynaPin::PlacementResult &)>;
    using CompletionCallback = std::function<void(const DynaPinPlacementSnapshot &,
                                                  const DynaPin::PlacementResult &,
                                                  bool canceled,
                                                  bool applied,
                                                  bool failed)>;

    explicit DynaPinPlacementJob(DynaPinPlacementSnapshot snapshot,
                                 ApplyCallback apply = {},
                                 CompletionCallback completion = {});

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &exception) override;

    const DynaPinPlacementSnapshot &snapshot() const { return m_snapshot; }
    const DynaPin::PlacementResult &result() const { return m_result; }
    bool applied() const { return m_applied; }

private:
    DynaPinPlacementSnapshot m_snapshot;
    ApplyCallback            m_apply;
    CompletionCallback       m_completion;
    DynaPin::PlacementResult m_result;
    bool                     m_applied = false;
};

} // namespace Slic3r::GUI

#endif // ORCASLICER_DYNAPIN_PLACEMENT_JOB_HPP
