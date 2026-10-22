#include "DynaPinLanding.hpp"
#include "ClipperUtils.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <memory>

namespace Slic3r::DynaPin {

double support_segment_speed(const Line& line, double speed, double min_x_distance, double max_x_speed)
{
    const double dx = std::abs(unscale<double>(line.b.x() - line.a.x()));
    if (dx < EPSILON || dx + EPSILON < min_x_distance)
        return speed;
    return std::min(speed, max_x_speed * unscale<double>(line.length()) / dx);
}

// Remove only marked paths; leave ordinary collections and their ordering intact.
static void extract_landing(ExtrusionEntityCollection& collection, ExtrusionPaths& landing)
{
    ExtrusionEntitiesPtr remaining;
    for (ExtrusionEntity* entity : collection.entities) {
        if (auto* nested = dynamic_cast<ExtrusionEntityCollection*>(entity)) {
            extract_landing(*nested, landing);
            if (!nested->empty()) {
                remaining.push_back(entity);
                continue;
            }
        } else if (auto* path = dynamic_cast<ExtrusionPath*>(entity)) {
            if (!path->is_dynapin_landing) {
                remaining.push_back(entity);
                continue;
            }
            landing.push_back(*path);
        } else if (auto* multi = dynamic_cast<ExtrusionMultiPath*>(entity)) {
            if (std::none_of(multi->paths.begin(), multi->paths.end(), [](const auto& p) { return p.is_dynapin_landing; })) {
                remaining.push_back(entity);
                continue;
            }
            for (const auto& path : multi->paths)
                if (path.is_dynapin_landing)
                    landing.push_back(path);
                else
                    remaining.push_back(path.clone());
        } else {
            remaining.push_back(entity);
            continue;
        }
        delete entity;
    }
    collection.entities = std::move(remaining);
}

static bool same_flow(const ExtrusionPath& a, const ExtrusionPath& b)
{ return a.role() == b.role() && a.height == b.height && a.width == b.width && a.mm3_per_mm == b.mm3_per_mm; }

// Merge continuous flow fragments without losing their individual extrusion parameters.
static void append_connected(ExtrusionEntityCollection& out, const ExtrusionPath& path)
{
    if (!out.empty() && out.last_point() == path.first_point()) {
        if (auto* previous = dynamic_cast<ExtrusionPath*>(out.entities.back())) {
            if (same_flow(*previous, path)) {
                append(previous->polyline.points, Points(path.polyline.points.begin() + 1, path.polyline.points.end()));
                return;
            }
            auto multi = std::make_unique<ExtrusionMultiPath>(*previous);
            multi->paths.push_back(path);
            multi->set_reverse();
            delete out.entities.back();
            out.entities.back() = multi.release();
            return;
        }
        if (auto* multi = dynamic_cast<ExtrusionMultiPath*>(out.entities.back())) {
            multi->paths.push_back(path);
            return;
        }
    }
    out.append(path);
}

// At generation time create links; after modulation reuse only the surviving,
// flow-correct fragments of those links. Never invent extrusion after modulation.
static void assemble_landing(ExtrusionEntityCollection& collection, const Polygons& regions, bool create_links, double inset_spacing = 0.)
{
    ExtrusionPaths landing;
    extract_landing(collection, landing);
    for (const ExPolygon& region : union_ex(regions)) {
        const Polygons                    boundary = to_polygons(region);
        std::map<coord_t, ExtrusionPaths> columns;
        ExtrusionPaths                    links;
        for (const auto& path : landing) {
            for (const Polyline& line : intersection_pl(path.polyline, boundary)) {
                for (size_t i = 1; i < line.points.size(); ++i) {
                    ExtrusionPath segment(Polyline(Points{line.points[i - 1], line.points[i]}), path);
                    if (segment.first_point() == segment.last_point())
                        continue;
                    if (segment.first_point().x() == segment.last_point().x())
                        columns[segment.first_point().x()].push_back(std::move(segment));
                    else {
                        if (segment.first_point().x() > segment.last_point().x())
                            segment.reverse();
                        links.push_back(std::move(segment));
                    }
                }
            }
        }
        auto ordered       = std::make_unique<ExtrusionEntityCollection>();
        ordered->no_sort   = true;
        bool    forward    = true;
        coord_t previous_x = 0;
        for (auto& [x, paths] : columns) {
            for (auto& path : paths) {
                if ((path.first_point().y() > path.last_point().y()) == forward)
                    path.reverse();
                path.set_reverse();
                path.is_dynapin_landing = true;
            }
            std::stable_sort(paths.begin(), paths.end(), [forward](const auto& a, const auto& b) {
                return forward ? a.first_point().y() < b.first_point().y() : a.first_point().y() > b.first_point().y();
            });
            for (size_t i = 0; i < paths.size(); ++i) {
                auto& path = paths[i];
                if (i == 0 && !ordered->empty() && x > previous_x) {
                    const Point start = ordered->last_point(), end = path.first_point();
                    if (create_links) {
                        const Polyline link(Points{start, end});
                        // Short local links, entirely inside the half-spacing inset.
                        const Polygons inset = offset(boundary, -0.5 * inset_spacing + SCALED_EPSILON);
                        if ((end - start).cast<double>().norm() <= 2. * double(x - previous_x) && diff_pl(link, inset).empty()) {
                            ExtrusionPath connector(link, path);
                            append_connected(*ordered, connector);
                        }
                    } else {
                        Point               current = start;
                        std::vector<size_t> chain;
                        while (current != end) {
                            const auto next = std::find_if(links.begin(), links.end(), [&](const auto& link) {
                                return link.first_point() == current && link.last_point().x() <= end.x();
                            });
                            if (next == links.end())
                                break;
                            chain.push_back(size_t(next - links.begin()));
                            current = next->last_point();
                        }
                        if (current == end)
                            for (size_t index : chain)
                                append_connected(*ordered, links[index]);
                    }
                }
                append_connected(*ordered, path);
            }
            previous_x = x;
            forward    = !forward;
        }
        if (!ordered->empty())
            collection.entities.push_back(ordered.release());
    }
}

void fill_landing_paths(ExtrusionEntitiesPtr& out, const Polygons& regions, const Flow& flow, double density, coord_t origin_x)
{
    if (regions.empty() || density <= 0.)
        return;
    const coord_t  pitch = std::max<coord_t>(1, coord_t(scale_(flow.spacing()) / density));
    const Polygons inset = offset(regions, -0.5 * flow.scaled_width());
    if (inset.empty())
        return;
    const BoundingBox box   = get_extents(inset);
    const coord_t     first = origin_x + coord_t(std::ceil(double(box.min.x() - origin_x) / pitch)) * pitch;
    Polylines         lines;
    for (coord_t x = first; x <= box.max.x(); x += pitch)
        lines.emplace_back(Points{Point(x, box.min.y() - coord_t(SCALED_EPSILON)), Point(x, box.max.y() + coord_t(SCALED_EPSILON))});
    Polylines tracks = intersection_pl(lines, inset);
    // A printable island narrower than the raster pitch may fall between all
    // grid columns. Give it one track without changing the traversal direction.
    for (const auto& island : union_ex(inset)) {
        const auto    bounds = get_extents(island);
        const coord_t x      = origin_x + coord_t(std::ceil(double(bounds.min.x() - origin_x) / pitch)) * pitch;
        if (x > bounds.max.x()) {
            const coord_t  center = bounds.min.x() + (bounds.max.x() - bounds.min.x()) / 2;
            const Polyline track(
                Points{Point(center, bounds.min.y() - coord_t(SCALED_EPSILON)), Point(center, bounds.max.y() + coord_t(SCALED_EPSILON))});
            append(tracks, intersection_pl(Polylines{track}, ExPolygons{island}));
        }
    }
    ExtrusionEntityCollection raw;
    extrusion_entities_append_paths(raw.entities, tracks, erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height(), false);
    for (auto* entity : raw.entities)
        static_cast<ExtrusionPath*>(entity)->is_dynapin_landing = true;
    assemble_landing(raw, regions, true, flow.scaled_width());
    // The existing modulator accepts flat ExtrusionPaths, and preserves their traversal.
    auto flat = raw.flatten();
    append(out, std::move(flat.entities));
    flat.entities.clear();
}

void order_landing_paths(ExtrusionEntityCollection& collection, const Polygons& regions)
{
    if (!regions.empty())
        assemble_landing(collection, regions, false);
}

void collect_landing_paths(const ExtrusionEntityCollection& collection, ExtrusionPaths& out)
{
    for (const ExtrusionEntity* entity : collection.entities) {
        if (const auto* nested = dynamic_cast<const ExtrusionEntityCollection*>(entity))
            collect_landing_paths(*nested, out);
        else if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity)) {
            if (path->is_dynapin_landing)
                out.push_back(*path);
        } else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
            for (const auto& path : multi->paths)
                if (path.is_dynapin_landing)
                    out.push_back(path);
        }
    }
}

std::vector<LandingExtrusion> schedule_landing_paths(std::vector<LandingExtrusion> paths, bool include_source_links)
{
    std::map<coord_t, std::vector<LandingExtrusion>> columns;
    std::vector<LandingExtrusion>                    links;
    for (const auto& item : paths) {
        const Points& points = item.path.polyline.points;
        for (size_t i = 1; i < points.size(); ++i) {
            if (points[i - 1] == points[i])
                continue;
            if (points[i - 1].x() != points[i].x()) {
                if (!include_source_links)
                    continue;
                ExtrusionPath link(Polyline(Points{points[i - 1], points[i]}), item.path);
                if (link.first_point().x() > link.last_point().x())
                    link.reverse();
                link.set_reverse();
                links.push_back({item.source, std::move(link)});
                continue;
            }
            ExtrusionPath segment(Polyline(Points{points[i - 1], points[i]}), item.path);
            segment.set_reverse();
            columns[segment.first_point().x()].push_back({item.source, std::move(segment)});
        }
    }

    // Per-object diagonal links may jump past another object's columns once the
    // columns are interleaved. Keep only the scan columns and form one global
    // serpentine order so travel moves connect adjacent columns at the same Y end.
    std::vector<LandingExtrusion> ordered;
    bool                          forward = true;
    for (auto& column_entry : columns) {
        auto& column = column_entry.second;
        std::stable_sort(column.begin(), column.end(), [forward](const auto& a, const auto& b) {
            const coord_t a_edge = forward ? std::min(a.path.first_point().y(), a.path.last_point().y()) :
                                             std::max(a.path.first_point().y(), a.path.last_point().y());
            const coord_t b_edge = forward ? std::min(b.path.first_point().y(), b.path.last_point().y()) :
                                             std::max(b.path.first_point().y(), b.path.last_point().y());
            return forward ? a_edge < b_edge : a_edge > b_edge;
        });
        for (auto& item : column) {
            const bool ascending = item.path.first_point().y() < item.path.last_point().y();
            if (ascending != forward)
                item.path.reverse();
            ordered.push_back(std::move(item));
        }
        forward = !forward;
    }

    // Preserve source links for callers that need the original extrusion volume.
    // Shared-layer G-code omits them because they jump past interleaved columns.
    ordered.insert(ordered.end(), std::make_move_iterator(links.begin()), std::make_move_iterator(links.end()));
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.path.first_point().x() != b.path.first_point().x())
            return a.path.first_point().x() < b.path.first_point().x();
        const bool a_column = a.path.first_point().x() == a.path.last_point().x();
        const bool b_column = b.path.first_point().x() == b.path.last_point().x();
        return a_column && !b_column;
    });
    return ordered;
}

bool ordered_landing_collection(const ExtrusionEntityCollection& collection)
{
    return collection.no_sort && !collection.empty() &&
           std::all_of(collection.entities.begin(), collection.entities.end(), [](const ExtrusionEntity* entity) {
               if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity))
                   return path->is_dynapin_landing;
               if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(entity))
                   return std::all_of(multi->paths.begin(), multi->paths.end(), [](const auto& path) { return path.is_dynapin_landing; });
               return false;
           });
}
} // namespace Slic3r::DynaPin
