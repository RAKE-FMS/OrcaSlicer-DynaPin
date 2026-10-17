#pragma once

// Frozen pre-optimization selector, used only as a differential test oracle.
#include "libslic3r/DynaPin.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Flow.hpp"
#include <algorithm>
#include <cmath>
#include <boost/log/trivial.hpp>

namespace Slic3r::Test {
using namespace DynaPin;
inline ProjectionSelection reference_select_from_projection(std::vector<ProjectionEvent> events)
{
    auto type_order = [](ProjectionEventType type) {
        switch (type) {
        case ProjectionEventType::Contact: return 0;
        case ProjectionEventType::PinSurface: return 1;
        case ProjectionEventType::Model: return 2;
        }
        return 3;
    };
    std::stable_sort(events.begin(), events.end(), [&type_order](const ProjectionEvent& lhs, const ProjectionEvent& rhs) {
        if (lhs.print_z != rhs.print_z)
            return lhs.print_z > rhs.print_z;
        return type_order(lhs.type) < type_order(rhs.type);
    });

    ProjectionSelection result;
    Polygons            projection;
    for (const ProjectionEvent& event : events) {
        if (event.type == ProjectionEventType::Contact) {
            polygons_append(projection, event.polygons);
            projection = union_(projection);
        } else if (event.type == ProjectionEventType::Model) {
            if (!projection.empty())
                projection = diff(projection, offset(event.polygons, float(SCALED_EPSILON)));
        } else if (!projection.empty() && !intersection(projection, event.polygons).empty()) {
            if (event.colliding)
                result.rejected_collisions.push_back(event.pin);
            else {
                result.selected.push_back(event.pin);
                // This is the downward-propagation termination point. A safe
                // upper pin consumes only the projection landing on its top.
                projection = diff(projection, event.polygons);
            }
        }
    }
    return result;
}

inline ProjectionSelection reference_select_tree_pins(const PrintObject&      object,
                                                      const Config&           config,
                                                      const std::vector<Pin>& candidates,
                                                      const std::vector<Pin>& colliding)
{
    std::vector<ProjectionEvent> events;
    std::vector<ProjectionEvent> contacts;
    events.reserve(object.layers().size() + candidates.size());
    for (size_t layer_id = 0; layer_id < object.layers().size(); ++layer_id) {
        const Layer& layer = *object.layers()[layer_id];
        if (layer_id == 0)
            continue;
        const Layer& lower    = *object.layers()[layer_id - 1];
        Polygons     overhang = diff(to_polygons(layer.lslices), offset(to_polygons(lower.lslices), scale_(0.05)));
        if (!overhang.empty()) {
            events.push_back({ProjectionEventType::Contact, layer.print_z, std::move(overhang), {}, false});
            contacts.push_back(events.back());
        }
    }
    for (const Pin& pin : candidates) {
        const VirtualSupportSurface surface = surface_for_pin(object, config, pin);
        events.push_back({ProjectionEventType::PinSurface,
                          surface.print_z,
                          {surface.poly},
                          pin,
                          std::find(colliding.begin(), colliding.end(), pin) != colliding.end()});
    }
    ProjectionSelection      selection     = reference_select_from_projection(std::move(events));
    const PrintObjectConfig& object_config = object.config();
    const double             layer_height  = std::max(object_config.layer_height.value, EPSILON);
    const double             angle         = object_config.tree_support_branch_angle.value * M_PI / 180.;
    double line_width = object_config.get_abs_value("support_line_width", object.print()->config().nozzle_diameter.get_at(0));
    if (line_width <= 0.)
        line_width = Flow::auto_extrusion_width(FlowRole::frSupportMaterial, float(object.print()->config().nozzle_diameter.get_at(0)));
    const double     move_per_layer    = std::min(std::tan(angle) * layer_height, line_width);
    std::vector<Pin> nearby_candidates = candidates;
    std::sort(nearby_candidates.begin(), nearby_candidates.end(),
              [](const Pin& lhs, const Pin& rhs) { return std::tie(lhs.row, lhs.col) > std::tie(rhs.row, rhs.col); });
    for (const Pin& pin : nearby_candidates) {
        if (std::find(colliding.begin(), colliding.end(), pin) != colliding.end() ||
            std::find(selection.selected.begin(), selection.selected.end(), pin) != selection.selected.end())
            continue;
        const VirtualSupportSurface surface = surface_for_pin(object, config, pin);
        for (const ProjectionEvent& contact : contacts) {
            if (contact.print_z <= surface.print_z + EPSILON)
                continue;
            Polygons remaining = contact.polygons;
            bool     shadowed  = false;
            for (const Pin& selected_pin : selection.selected) {
                const VirtualSupportSurface selected_surface = surface_for_pin(object, config, selected_pin);
                if (selected_surface.print_z > surface.print_z + EPSILON && selected_surface.print_z < contact.print_z - EPSILON) {
                    if (!intersection({selected_surface.poly}, {surface.poly}).empty() &&
                        !intersection(contact.polygons, {selected_surface.poly}).empty())
                        shadowed = true;
                    remaining = diff(remaining, {selected_surface.poly});
                }
            }
            if (shadowed || remaining.empty())
                continue;
            const double travel = move_per_layer * std::ceil((contact.print_z - surface.print_z) / layer_height);
            if (!intersection(offset(remaining, float(scale_(travel))), {surface.poly}).empty()) {
                selection.selected.push_back(pin);
                break;
            }
        }
    }
    sort_unique_pins(selection.selected, config);
    return selection;
}

static std::optional<double> reference_bed_max_x(const Print& print)
{
    const Points bed = get_bed_shape(print.config());
    if (bed.empty())
        return std::nullopt;

    coord_t max_x = bed.front().x();
    for (const Point& point : bed)
        max_x = std::max(max_x, point.x());
    return unscale_(max_x);
}
static LocalBlocker reference_blocker_for_pin_shift(const Config& config, const Pin& pin, const Point& shift, double max_x_bed)
{
    const double        y       = pin_y(config, pin);
    const BlockerZRange z_range = blocker_z_range(config, pin);
    const double        block_y = y - 7.2;
    const double        min_x   = support_x_front(config);
    const double        max_x   = max_x_bed;
    const double        min_y   = block_y - 0.5 * config.blocker_width_y;
    const double        max_y   = block_y + 0.5 * config.blocker_width_y;

    LocalBlocker blocker;
    blocker.pin         = pin;
    blocker.z_min       = z_range.z_min;
    blocker.z_max       = z_range.z_max;
    blocker.poly.points = {Point(scale_(min_x) - shift.x(), scale_(min_y) - shift.y()),
                           Point(scale_(max_x) - shift.x(), scale_(min_y) - shift.y()),
                           Point(scale_(max_x) - shift.x(), scale_(max_y) - shift.y()),
                           Point(scale_(min_x) - shift.x(), scale_(max_y) - shift.y())};
    return blocker;
}
inline bool reference_pin_collides_with_model(const Print& print, const Config& config, const Pin& pin)
{
    const std::optional<double> max_x_bed = reference_bed_max_x(print);
    if (!max_x_bed) {
        BOOST_LOG_TRIVIAL(warning) << "[DynaPin] cannot check pin collision without a printable bed X range";
        return true;
    }
    for (const PrintObject* object : print.objects()) {
        const auto check_instance = [&](const Point& shift) {
            const LocalBlocker blocker = reference_blocker_for_pin_shift(config, pin, shift, *max_x_bed);
            for (const Layer* layer : object->layers()) {
                if (layer->print_z + EPSILON < blocker.z_min || layer->print_z - EPSILON > blocker.z_max)
                    continue;
                const Polygons overlap = intersection(to_polygons(layer->lslices), {blocker.poly});
                if (!overlap.empty()) {
                    BOOST_LOG_TRIVIAL(debug) << "[DynaPin] model collision: pin=(" << pin.row << "," << pin.col << ")"
                                             << ", layer_id=" << layer->id() << ", layer_z=" << layer->print_z << ", instance_shift_mm=("
                                             << unscale<double>(shift.x()) << "," << unscale<double>(shift.y()) << ")"
                                             << ", blocker_x_mm=[" << unscale<double>(blocker.poly.points.front().x() + shift.x()) << ","
                                             << unscale<double>(blocker.poly.points[1].x() + shift.x()) << "]"
                                             << ", overlap_area_mm2=" << area(overlap) * SCALING_FACTOR * SCALING_FACTOR;
                    return true;
                }
            }
            return false;
        };

        if (object->instances().empty()) {
            if (check_instance(Point(0, 0)))
                return true;
        } else {
            for (const PrintInstance& instance : object->instances()) {
                if (check_instance(instance.shift_without_plate_offset()))
                    return true;
            }
        }
    }
    return false;
}

} // namespace Slic3r::Test
