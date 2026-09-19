#ifndef slic3r_SliceStatisticsLog_hpp_
#define slic3r_SliceStatisticsLog_hpp_

#include <nlohmann/json.hpp>
#include <string>

namespace Slic3r {

struct GCodeProcessorResult;
struct PrintStatistics;

bool should_log_slice_statistics(bool slice_succeeded, bool is_fff, const GCodeProcessorResult* result, unsigned int last_logged_result_id);

// Build one self-contained log record from the same statistics used by the GUI preview.
nlohmann::ordered_json make_slice_statistics_log(const GCodeProcessorResult& result,
                                         const PrintStatistics&      print_statistics,
                                         int                         plate_index,
                                         bool                        imperial_units);

std::string slice_statistics_sidecar_path(const std::string& gcode_path);
void        write_slice_statistics_sidecar(const std::string& gcode_path, const nlohmann::ordered_json& statistics);

} // namespace Slic3r

#endif
