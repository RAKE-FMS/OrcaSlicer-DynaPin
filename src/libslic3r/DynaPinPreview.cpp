#include "DynaPinPreview.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <regex>
#include <string_view>

namespace Slic3r {
namespace {

struct PendingEvent
{
    DynaPinAddress address;
    size_t         begin_line{0};
    size_t         pull_line{0};
    bool           has_pull{false};
};

bool contains(std::string_view text, std::string_view needle) { return text.find(needle) != std::string_view::npos; }

std::optional<DynaPinAddress> parse_begin_comment(std::string_view line)
{
    static const std::regex begin_re(R"(^\s*;\s*BEGIN_DYNAPIN_PULL\s+ROW=(\d+)\s+COL=(\d+)\s*$)");
    std::cmatch             match;
    if (!std::regex_match(line.data(), line.data() + line.size(), match, begin_re))
        return std::nullopt;

    return DynaPinAddress{std::stoi(match[1].str()), std::stoi(match[2].str())};
}

unsigned int gcode_id_for_line(const std::vector<GCodeProcessorResult::MoveVertex>& moves, size_t line_id)
{
    unsigned int best = 0;
    for (const GCodeProcessorResult::MoveVertex& move : moves) {
        if (move.gcode_id <= line_id)
            best = std::max(best, move.gcode_id);
        else
            break;
    }
    return best;
}

unsigned int first_gcode_id_at_or_after_line(const std::vector<GCodeProcessorResult::MoveVertex>& moves, size_t line_id)
{
    for (const GCodeProcessorResult::MoveVertex& move : moves)
        if (move.gcode_id >= line_id)
            return move.gcode_id;
    return 0;
}

const GCodeProcessorResult::MoveVertex* move_for_gcode_id(const std::vector<GCodeProcessorResult::MoveVertex>& moves, unsigned int gcode_id)
{
    auto it = std::find_if(moves.begin(), moves.end(),
                           [gcode_id](const GCodeProcessorResult::MoveVertex& move) { return move.gcode_id == gcode_id; });
    return it == moves.end() ? nullptr : &*it;
}

Vec3f previous_position_for_gcode_id(const std::vector<GCodeProcessorResult::MoveVertex>& moves,
                                     unsigned int                                         gcode_id,
                                     const Vec3f&                                         fallback)
{
    Vec3f previous = fallback;
    for (const GCodeProcessorResult::MoveVertex& move : moves) {
        if (move.gcode_id >= gcode_id)
            break;
        previous = move.position;
    }
    return previous;
}

std::vector<std::string> read_gcode_lines(const GCodeProcessorResult& result)
{
    std::ifstream file(result.filename, std::ios::binary);
    if (!file)
        return {};

    if (result.lines_ends.empty()) {
        std::vector<std::string> lines;
        std::string              line;
        while (std::getline(file, line))
            lines.push_back(line);
        return lines;
    }

    std::string              contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::string> lines;
    lines.reserve(result.lines_ends.size());

    size_t start = 0;
    for (size_t end : result.lines_ends) {
        if (end > contents.size())
            break;
        size_t line_end = end;
        if (line_end > start && contents[line_end - 1] == '\n')
            --line_end;
        if (line_end > start && contents[line_end - 1] == '\r')
            --line_end;
        lines.emplace_back(contents.substr(start, line_end - start));
        start = end;
    }

    return lines;
}

} // namespace

void DynaPinPreviewState::reset()
{
    m_events.clear();
    m_selection.reset();
}

void DynaPinPreviewState::load(const GCodeProcessorResult& result)
{
    m_events.clear();
    m_selection.reset();

    if (result.filename.empty() || result.moves.empty())
        return;

    std::vector<std::string> lines = read_gcode_lines(result);
    if (lines.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "DynaPin preview: failed to open G-code file " << result.filename;
        return;
    }

    std::optional<PendingEvent> pending;
    for (size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
        const size_t       line_id = line_idx + 1;
        const std::string& line    = lines[line_idx];

        if (!pending) {
            if (std::optional<DynaPinAddress> address = parse_begin_comment(line))
                pending = PendingEvent{*address, line_id, 0, false};
            continue;
        }

        if (line.find("; DYNAPIN_PULL_MOVE") != std::string::npos) {
            pending->pull_line = line_id;
            pending->has_pull  = true;
            continue;
        }

        if (line.find("; END_DYNAPIN_PULL") == std::string::npos)
            continue;

        if (!pending->has_pull) {
            BOOST_LOG_TRIVIAL(warning) << "DynaPin preview: ignoring block without DYNAPIN_PULL_MOVE at line " << pending->begin_line;
            pending.reset();
            continue;
        }

        const unsigned int                      begin_gcode_id = gcode_id_for_line(result.moves, pending->begin_line);
        const unsigned int                      pull_gcode_id  = first_gcode_id_at_or_after_line(result.moves, pending->pull_line);
        const unsigned int                      end_gcode_id   = gcode_id_for_line(result.moves, line_id);
        const GCodeProcessorResult::MoveVertex* pull_move      = move_for_gcode_id(result.moves, pull_gcode_id);
        if (pull_gcode_id == 0 || pull_move == nullptr) {
            BOOST_LOG_TRIVIAL(warning) << "DynaPin preview: ignoring block with unresolved pull move at line " << pending->begin_line;
            pending.reset();
            continue;
        }

        const Vec3f start_pos = previous_position_for_gcode_id(result.moves, pull_gcode_id, pull_move->position);
        m_events.push_back({pending->address, begin_gcode_id, pull_gcode_id, end_gcode_id, start_pos, pull_move->position});
        pending.reset();
    }

    if (pending)
        BOOST_LOG_TRIVIAL(warning) << "DynaPin preview: ignoring incomplete block at line " << pending->begin_line;

    // Do not select a pin by default to avoid showing the nozzle preview initially.
    // if (!m_events.empty())
    //     m_selection = DynaPinSelection{m_events.front().address, m_events.front().start_pos};
}

Vec3f DynaPinPreviewState::position_for_gcode_id(unsigned int gcode_id) const
{
    if (!m_selection)
        return Vec3f::Zero();

    Vec3f position = m_selection->original_pos;
    for (const DynaPinEvent& event : m_events) {
        if (!(event.address == m_selection->address))
            continue;

        if (gcode_id < event.pull_gcode_id)
            return position;

        if (gcode_id == event.pull_gcode_id) {
            position = event.end_pos;
            continue;
        }

        position = event.end_pos;
    }

    return position;
}

std::optional<DynaPinAddress> DynaPinPreviewState::parse_model_name(const std::string& name)
{
    static const std::regex name_re(R"(^dynapin_r(\d+)_c(\d+)$)", std::regex::icase);
    std::smatch             match;
    if (!std::regex_match(name, match, name_re))
        return std::nullopt;

    return DynaPinAddress{std::stoi(match[1].str()), std::stoi(match[2].str())};
}

} // namespace Slic3r
