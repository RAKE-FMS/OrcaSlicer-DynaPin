#include "DynaPinPlacementJob.hpp"

#include <exception>
#include <utility>

namespace Slic3r::GUI {

DynaPinPlacementJob::DynaPinPlacementJob(DynaPinPlacementSnapshot snapshot, ApplyCallback apply, CompletionCallback completion) :
    m_snapshot(std::move(snapshot)),
    m_apply(std::move(apply)),
    m_completion(std::move(completion))
{
}

void DynaPinPlacementJob::process(Ctl &ctl)
{
    ctl.update_status(0, "Searching for an improved DynaPin placement");

    std::string eligibility_error;
    if (!m_snapshot.eligibility.valid(&eligibility_error)) {
        m_result.status  = DynaPin::PlacementStatus::InvalidConfig;
        m_result.warning = std::move(eligibility_error);
        ctl.update_status(100, m_result.warning);
        return;
    }

    DynaPin::PlacementEvaluator evaluator = m_snapshot.evaluator;
    if (!evaluator && m_snapshot.scene.model)
        evaluator = DynaPin::make_scene_evaluator(m_snapshot.scene, [&ctl]() { return ctl.was_canceled(); });

    m_result = DynaPin::optimize_placement(
        m_snapshot.search,
        evaluator,
        [&ctl]() { return ctl.was_canceled(); },
        [&ctl](int progress) { ctl.update_status(progress, "Searching for an improved DynaPin placement"); });

    if (m_result.status == DynaPin::PlacementStatus::Canceled || ctl.was_canceled())
        ctl.update_status(100, "DynaPin placement search canceled");
    else
        ctl.update_status(100, "DynaPin placement search finished");
}

void DynaPinPlacementJob::finalize(bool canceled, std::exception_ptr &exception)
{
    if (!canceled && !exception && m_result.status == DynaPin::PlacementStatus::Improved && m_apply) {
        // The callback runs on the UI thread.  It is responsible for checking
        // the target ID, input generation, and current model/config signature
        // before touching the live model; a stale result is therefore
        // discarded safely.
        m_applied = m_apply(m_snapshot, m_result);
    }

    if (m_completion)
        m_completion(m_snapshot, m_result, canceled, m_applied, exception != nullptr);
}

} // namespace Slic3r::GUI
