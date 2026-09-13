#include "DynaPinPlacementJob.hpp"

#include <atomic>
#include <exception>
#include <utility>

namespace Slic3r::GUI {
namespace {

const char *placement_progress_message(DynaPin::PlacementProgressStage stage)
{
    switch (stage) {
    case DynaPin::PlacementProgressStage::EvaluatingCurrent:   return "Evaluating the current DynaPin placement";
    case DynaPin::PlacementProgressStage::PreparingCandidates: return "Preparing DynaPin placement candidates";
    case DynaPin::PlacementProgressStage::CoarseSearch:        return "Searching for an improved DynaPin placement";
    case DynaPin::PlacementProgressStage::LocalSearch:         return "Refining the DynaPin placement";
    case DynaPin::PlacementProgressStage::Finalizing:          return "Finishing the DynaPin placement search";
    }
    return "Searching for an improved DynaPin placement";
}

} // namespace

DynaPinPlacementJob::DynaPinPlacementJob(DynaPinPlacementSnapshot snapshot, ApplyCallback apply, CompletionCallback completion) :
    m_snapshot(std::move(snapshot)),
    m_apply(std::move(apply)),
    m_completion(std::move(completion))
{
}

void DynaPinPlacementJob::process(Ctl &ctl)
{
    std::atomic<int>                             progress_value{0};
    std::atomic<DynaPin::PlacementProgressStage> progress_stage{DynaPin::PlacementProgressStage::EvaluatingCurrent};
    ctl.update_status(0, placement_progress_message(progress_stage.load()));

    std::string eligibility_error;
    if (!m_snapshot.eligibility.valid(&eligibility_error)) {
        m_result.status  = DynaPin::PlacementStatus::InvalidConfig;
        m_result.warning = std::move(eligibility_error);
        ctl.update_status(100, m_result.warning);
        return;
    }

    DynaPin::PlacementEvaluator evaluator = m_snapshot.evaluator;
    if (!evaluator && m_snapshot.scene.model) {
        if (!m_snapshot.search.angle_evaluator)
            m_snapshot.search.angle_evaluator = DynaPin::make_scene_angle_evaluator(
                m_snapshot.scene, [&ctl]() { return ctl.was_canceled(); });
        evaluator = DynaPin::make_scene_evaluator(m_snapshot.scene, [&ctl]() { return ctl.was_canceled(); });
    }

    m_result = DynaPin::optimize_placement(
        m_snapshot.search,
        evaluator,
        [&ctl]() { return ctl.was_canceled(); },
        [&ctl, &progress_value, &progress_stage](int progress) {
            progress_value.store(progress, std::memory_order_relaxed);
            ctl.update_status(progress, placement_progress_message(progress_stage.load(std::memory_order_relaxed)));
        },
        [&ctl, &progress_value, &progress_stage](DynaPin::PlacementProgressStage stage) {
            progress_stage.store(stage, std::memory_order_relaxed);
            ctl.update_status(progress_value.load(std::memory_order_relaxed), placement_progress_message(stage));
        });

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
