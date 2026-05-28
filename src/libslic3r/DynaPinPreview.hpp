#ifndef slic3r_DynaPinPreview_hpp_
#define slic3r_DynaPinPreview_hpp_

#include "GCode/GCodeProcessor.hpp"
#include "Point.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

struct DynaPinAddress
{
    int row{ -1 };
    int col{ -1 };

    bool operator==(const DynaPinAddress& rhs) const { return row == rhs.row && col == rhs.col; }
};

struct DynaPinEvent
{
    DynaPinAddress address;
    unsigned int   begin_gcode_id{ 0 };
    unsigned int   pull_gcode_id{ 0 };
    unsigned int   end_gcode_id{ 0 };
    Vec3f          start_pos{ Vec3f::Zero() };
    Vec3f          end_pos{ Vec3f::Zero() };
};

struct DynaPinSelection
{
    DynaPinAddress address;
    Vec3f          original_pos{ Vec3f::Zero() };
};

class DynaPinPreviewState
{
public:
    void reset();
    void load(const GCodeProcessorResult& result);

    const std::vector<DynaPinEvent>& events() const { return m_events; }
    bool empty() const { return m_events.empty(); }

    void set_selection(const DynaPinSelection& selection) { m_selection = selection; }
    void clear_selection() { m_selection.reset(); }
    const std::optional<DynaPinSelection>& selection() const { return m_selection; }

    Vec3f position_for_gcode_id(unsigned int gcode_id) const;

    static std::optional<DynaPinAddress> parse_model_name(const std::string& name);

private:
    std::vector<DynaPinEvent>       m_events;
    std::optional<DynaPinSelection> m_selection;
};

} // namespace Slic3r

#endif // slic3r_DynaPinPreview_hpp_
