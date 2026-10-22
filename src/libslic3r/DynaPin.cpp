#include "DynaPin.hpp"
#include "DynaPinSupport.hpp"
#include "DynaPinGCode.hpp"
#include "DynaPinLanding.hpp"
#include "ExtrusionEntityCollection.hpp"

#include "Layer.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <initializer_list>
#include <utility>
#include <map>
#include <numeric>
#include <chrono>
#include <cstdlib>
#include <set>
#include <sstream>

namespace fs = boost::filesystem;

namespace Slic3r::DynaPin {

static ExtrusionPaths support_paths(const ExtrusionEntityCollection& collection)
{
    ExtrusionPaths paths;
    const auto     flat = collection.flatten();
    for (const auto* entity : flat.entities) {
        if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity))
            paths.push_back(*path);
        else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(entity))
            append(paths, multi->paths);
        else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity))
            append(paths, loop->paths);
    }
    return paths;
}

bool starts_on_pin(const PrintObject& object)
{
    if (!object.print()->config().enable_dynapin_support_optimization.value || effective_debug_stage(*object.print()) < 1)
        return false;
    const SupportLayer* first = nullptr;
    for (const auto* layer : object.support_layers())
        if (layer->has_extrusions() && (!first || layer->print_z < first->print_z))
            first = layer;
    if (!first)
        return false;
    for (const auto* layer : object.layers())
        if (layer->has_extrusions() && layer->print_z < first->print_z - EPSILON)
            return false;
    ExtrusionPaths paths;
    collect_landing_paths(first->support_fills, paths);
    const auto surfaces = landing_surfaces_for_object(object);
    for (const auto& path : paths)
        for (const auto& surface : surfaces)
            if (std::abs(first->print_z - path.height - surface.print_z) < EPSILON &&
                surface.poly.contains((path.first_point() + path.last_point()) / 2))
                return true;
    return false;
}

std::vector<SupportExtrusions> normalized_support_paths(const std::vector<PrintObject*>& objects)
{
    std::vector<SupportExtrusions> result;
    using Group = std::map<size_t, ExtrusionEntityCollection>;
    std::map<std::pair<coord_t, unsigned int>, Group> groups;
    std::vector<double>                               densities;
    for (const auto* object : objects) {
        const auto  tools = object->object_extruders();
        const Point shift = object->instances().front().shift;
        for (const auto* layer : object->support_layers()) {
            const size_t source = result.size();
            result.push_back({object, layer, {}});
            densities.push_back(object->config().raft_first_layer_density.value * 0.01);
            for (auto& path : support_paths(layer->support_fills)) {
                unsigned int filament = (path.role() == erSupportMaterialInterface || path.role() == erIroning) ?
                                            object->config().support_interface_filament.value :
                                            object->config().support_filament.value;
                if (filament == 0 && tools.size() != 1) {
                    // Export may inherit the current tool. Keep ambiguous cases
                    // unchanged so collision checking remains conservative.
                    path.polyline.translate(shift);
                    result.back().paths.push_back(std::move(path));
                    continue;
                }
                filament = filament == 0 ? tools.front() : filament - 1;
                groups[{coord_t(std::llround(layer->print_z / EPSILON)), filament}][source].append(path);
            }
        }
    }
    for (auto& layer_group : groups) {
        std::vector<LandingExtrusion> emitted, landing;
        std::vector<LandingRegion>    regions;
        size_t                        landing_sources = 0;
        for (auto& entry : layer_group.second) {
            const auto& source = result[entry.first];
            const Point shift  = source.object->instances().front().shift;
            remove_duplicate_support_paths(entry.second, shift, entry.first, erMixed, emitted);
            ExtrusionPaths paths;
            collect_landing_paths(entry.second, paths);
            if (!paths.empty()) {
                ++landing_sources;
                append(regions, landing_extrusion_regions(*source.layer, shift, entry.first));
            }
            for (auto& path : paths) {
                path.polyline.translate(shift);
                landing.push_back({entry.first, std::move(path)});
            }
        }
        const bool shared = landing_sources > 1;
        if (shared)
            for (auto& item : schedule_landing_paths(std::move(landing), false, densities, regions))
                result[item.source].paths.push_back(std::move(item.path));
        for (const auto& entry : layer_group.second) {
            auto& source = result[entry.first];
            for (auto& path : support_paths(entry.second)) {
                if (shared && path.is_dynapin_landing)
                    continue;
                path.polyline.translate(source.object->instances().front().shift);
                source.paths.push_back(std::move(path));
            }
        }
    }
    return result;
}

std::vector<LandingRegion> landing_extrusion_regions(const SupportLayer& layer, const Point& instance_shift, size_t source)
{
    std::vector<LandingRegion> result;
    for (const auto& surface : landing_surfaces_for_object(*layer.object())) {
        const float height = float(layer.print_z - surface.print_z);
        if (height <= EPSILON)
            continue;
        Polygons region = intersection(layer.dynapin_landing_regions, Polygons{surface.poly});
        if (!region.empty()) {
            for (auto& polygon : region)
                polygon.translate(instance_shift);
            result.push_back({source, height, std::move(region)});
        }
    }
    return result;
}

static ExtrusionPaths unique_support_path(const ExtrusionPath&           path,
                                          const Point&                   shift,
                                          size_t                         source,
                                          std::vector<LandingExtrusion>& emitted)
{
    ExtrusionPath machine_path = path;
    machine_path.polyline.translate(shift);
    Polylines kept;
    bool      changed = false;
    for (const Line& segment : machine_path.polyline.lines()) {
        Polygons overlaps;
        for (const auto& previous : emitted) {
            const auto& other = previous.path;
            if (previous.source == source || path.role() != other.role() || path.height != other.height || path.width != other.width ||
                path.mm3_per_mm != other.mm3_per_mm)
                continue;
            for (const Line& printed : other.polyline.lines()) {
                // Do not cut transverse intersections or adjacent scan lines.
                if (!segment.parallel_to(printed) ||
                    Line::distance_to_infinite_squared(printed.a, segment.a, segment.b) > double(SCALED_EPSILON) * SCALED_EPSILON ||
                    Line::distance_to_infinite_squared(printed.b, segment.a, segment.b) > double(SCALED_EPSILON) * SCALED_EPSILON)
                    continue;
                polygons_append(overlaps, offset(Polyline(Points{printed.a, printed.b}), SCALED_EPSILON, ClipperLib::jtSquare, 2.,
                                                 ClipperLib::etOpenButt));
            }
        }
        const Polyline original(Points{segment.a, segment.b});
        Polylines      parts = overlaps.empty() ? Polylines{original} : diff_pl(original, overlaps);
        changed |= parts.size() != 1 || std::abs(parts.front().length() - original.length()) > SCALED_EPSILON;
        for (auto& part : parts)
            if ((part.last_point() - segment.a).cast<double>().squaredNorm() < (part.first_point() - segment.a).cast<double>().squaredNorm())
                part.reverse();
        std::sort(parts.begin(), parts.end(), [&segment](const Polyline& a, const Polyline& b) {
            return (a.first_point() - segment.a).cast<double>().squaredNorm() < (b.first_point() - segment.a).cast<double>().squaredNorm();
        });
        for (auto& part : parts) {
            if (!kept.empty() && kept.back().last_point() == part.first_point())
                append(kept.back().points, Points(part.points.begin() + 1, part.points.end()));
            else
                kept.push_back(std::move(part));
        }
    }
    ExtrusionPaths result;
    if (!changed)
        result.push_back(path);
    else
        for (auto& part : kept) {
            part.translate(-shift);
            result.emplace_back(std::move(part), path);
        }
    for (const auto& part : result) {
        auto machine = part;
        machine.polyline.translate(shift);
        emitted.push_back({source, std::move(machine)});
    }
    return result;
}

void remove_duplicate_support_paths(ExtrusionEntityCollection&     collection,
                                    const Point&                   instance_shift,
                                    size_t                         source,
                                    ExtrusionRole                  role,
                                    std::vector<LandingExtrusion>& emitted)
{
    ExtrusionEntitiesPtr remaining;
    const auto           process_path = [&](const ExtrusionPath& path, ExtrusionEntityCollection& out) {
        if (path.role() == role || (role == erMixed && path.role() != erIroning)) {
            auto       parts   = unique_support_path(path, instance_shift, source, emitted);
            const bool changed = parts.size() != 1 || parts.front().polyline.points != path.polyline.points;
            out.append(std::move(parts));
            return changed;
        } else {
            out.append(path);
            return false;
        }
    };
    for (auto* entity : collection.entities) {
        if (auto* nested = dynamic_cast<ExtrusionEntityCollection*>(entity)) {
            remove_duplicate_support_paths(*nested, instance_shift, source, role, emitted);
            if (!nested->empty())
                remaining.push_back(entity);
            else
                delete entity;
        } else if (auto* path = dynamic_cast<ExtrusionPath*>(entity)) {
            ExtrusionEntityCollection out;
            process_path(*path, out);
            append(remaining, std::move(out.entities));
            delete entity;
        } else if (dynamic_cast<ExtrusionMultiPath*>(entity) || dynamic_cast<ExtrusionLoop*>(entity)) {
            const auto* multi = dynamic_cast<ExtrusionMultiPath*>(entity);
            const auto& paths = multi ? multi->paths : static_cast<ExtrusionLoop*>(entity)->paths;
            auto        out   = std::make_unique<ExtrusionEntityCollection>();
            out->no_sort      = true;
            bool changed      = false;
            for (const auto& path : paths)
                changed |= process_path(path, *out);
            if (!changed) {
                remaining.push_back(entity);
                continue;
            }
            if (!out->empty())
                remaining.push_back(out.release());
            delete entity;
        } else {
            remaining.push_back(entity);
        }
    }
    collection.entities = std::move(remaining);
}

std::vector<LandingLayerAdjustment> align_landing_layers(const std::vector<LandingLayer>&          layers,
                                                         const std::vector<VirtualSupportSurface>& surfaces,
                                                         double                                    min_height,
                                                         double                                    max_height,
                                                         double                                    expansion)
{
    std::vector<LandingLayerAdjustment> result;
    std::vector<Polygons>               assigned(layers.size());
    std::map<int, std::vector<Polygons>> expansion_seeds;
    if (expansion > 0.) {
        for (const auto& surface : surfaces) {
            const int key = surface.pin.row;
            if (surface.source_print_z == 0. || expansion_seeds.count(key) != 0)
                continue;
            std::vector<VirtualSupportSurface> bands;
            for (const auto& other : surfaces)
                if (other.pin.row == surface.pin.row)
                    bands.push_back(other);
            expansion_seeds.emplace(key, landing_regions(layers, bands, 1));
        }
    }
    std::vector<size_t> order(layers.size());
    std::iota(order.begin(), order.end(), 0);
    if (!expansion_seeds.empty())
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return layers[a].print_z < layers[b].print_z; });
    for (const VirtualSupportSurface& surface : surfaces) {
        auto       masks    = landing_regions(layers, {surface}, 1);
        const bool expanded = surface.source_print_z != 0. && expansion > 0.;
        if (expanded) {
            // Expand before cutting into bands, including bands with no seed.
            // Use actual first slabs, which may straddle the routing envelope.
            masks = expansion_seeds.at(surface.pin.row);
            for (auto& mask : masks)
                mask = intersection(offset(mask, scale_(expansion)), {surface.poly});
        }
        Polygons surface_assigned;
        for (size_t i : order) {
            if (masks[i].empty() || layers[i].bottom_z < surface.print_z - EPSILON ||
                (layers[i].bottom_z <= surface.print_z + EPSILON && (surface.source_print_z == 0. || expansion <= 0.)))
                continue;
            Polygons region = diff(masks[i], assigned[i]);
            if (expanded)
                region = diff(region, surface_assigned);
            if (region.empty())
                continue;
            const double span   = layers[i].print_z - surface.print_z;
            const size_t count  = size_t(std::ceil(span / max_height - EPSILON));
            const double height = span / double(count);
            // No printable layer fits between this pin and the upper contact.
            if (height < min_height - EPSILON)
                continue;
            LandingLayerAdjustment adjustment{i, {}};
            double                 bottom    = surface.print_z;
            const double           grid_step = max_height - min_height;
            if (surface.source_print_z != 0. && min_height > EPSILON && grid_step >= min_height - EPSILON) {
                // Share horizontal planes across bands instead of dividing each
                // band independently. The first layer absorbs the local slope;
                // subsequent layers use one grid and retain the original top.
                // Anchor at the original upper connection: every interval after
                // the first is exactly grid_step; the first remains in [min,max].
                const size_t steps = size_t(std::floor((span - min_height + EPSILON) / grid_step));
                for (size_t j = 0; j <= steps; ++j) {
                    const double top = j == steps ? layers[i].print_z : layers[i].print_z - double(steps - j) * grid_step;
                    adjustment.layers.push_back({bottom, top, region});
                    bottom = top;
                }
            } else {
                for (size_t j = 0; j < count; ++j) {
                    const double top = j + 1 == count ? layers[i].print_z : surface.print_z + double(j + 1) * height;
                    adjustment.layers.push_back({bottom, top, region});
                    bottom = top;
                }
            }
            assigned[i] = union_(assigned[i], region);
            if (expanded)
                surface_assigned = union_(surface_assigned, region);
            result.push_back(std::move(adjustment));
        }
    }
    return result;
}

std::vector<Polygons> landing_regions(const std::vector<LandingLayer>&          layers,
                                      const std::vector<VirtualSupportSurface>& surfaces,
                                      size_t                                    layer_count)
{
    std::vector<Polygons> result(layers.size());
    if (layer_count == 0 || surfaces.empty())
        return result;
    double max_layer_height = 0.;
    for (const LandingLayer& layer : layers)
        max_layer_height = std::max(max_layer_height, layer.print_z - layer.bottom_z);
    std::vector<size_t> order(layers.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return layers[a].print_z < layers[b].print_z; });
    for (const VirtualSupportSurface& surface : surfaces) {
        size_t count      = 0;
        double previous_z = surface.print_z;
        for (size_t i = 0; i < order.size() && count < layer_count;) {
            size_t j = i + 1;
            while (j < order.size() && layers[order[j]].print_z <= layers[order[i]].print_z + EPSILON)
                ++j;
            bool overlaps = false;
            for (size_t k = i; k < j; ++k) {
                const size_t id = order[k];
                if (layers[id].bottom_z < surface.print_z - EPSILON)
                    continue;
                if (count > 0 && layers[id].bottom_z > previous_z + EPSILON)
                    continue;
                // A clipped slab can leave a gap of at most one layer above
                // the pin. A remote column resting on a model is not a landing.
                if (count == 0 && layers[id].bottom_z > std::max(surface.print_z, surface.source_print_z) + max_layer_height + EPSILON)
                    continue;
                Polygons region = intersection(layers[id].polygons, {surface.poly});
                overlaps |= !region.empty();
                polygons_append(result[id], std::move(region));
            }
            if (overlaps) {
                ++count;
                previous_z = layers[order[i]].print_z;
            }
            i = j;
        }
    }
    for (Polygons& regions : result)
        if (!regions.empty())
            regions = union_(std::move(regions));
    return result;
}

std::vector<Polygons> stabilization_regions(const std::vector<LandingLayer>& layers, const std::vector<Polygons>& bases, size_t layer_count)
{
    assert(layers.size() == bases.size());
    std::vector<Polygons> result(layers.size());
    if (layer_count == 0)
        return result;
    std::vector<size_t> order(layers.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return layers[a].print_z < layers[b].print_z; });
    for (size_t base = 0; base < layers.size(); ++base) {
        if (bases[base].empty())
            continue;
        double previous_z = layers[base].print_z;
        size_t count      = 0;
        for (size_t i = 0; i < order.size() && count < layer_count;) {
            size_t j = i + 1;
            while (j < order.size() && layers[order[j]].print_z <= layers[order[i]].print_z + EPSILON)
                ++j;
            bool overlaps = false;
            for (size_t k = i; k < j; ++k) {
                const size_t id = order[k];
                if (layers[id].print_z <= layers[base].print_z + EPSILON || layers[id].bottom_z > previous_z + EPSILON)
                    continue;
                Polygons region = diff(intersection(layers[id].polygons, bases[base]), bases[id]);
                overlaps |= !region.empty();
                polygons_append(result[id], std::move(region));
            }
            if (overlaps) {
                ++count;
                previous_z = layers[order[i]].print_z;
            }
            i = j;
        }
    }
    for (Polygons& region : result)
        if (!region.empty())
            region = union_(std::move(region));
    return result;
}

std::vector<Polygons> support_stabilization_regions(const PrintObject& object, size_t layer_count)
{
    const auto&           support_layers = object.support_layers();
    std::vector<Polygons> result(support_layers.size());
    if (layer_count == 0 || std::none_of(support_layers.begin(), support_layers.end(),
                                         [](const SupportLayer* layer) { return !layer->dynapin_landing_regions.empty(); }))
        return result;
    const auto                surfaces = landing_surfaces_for_object(object);
    std::vector<LandingLayer> slabs;
    std::vector<Polygons>     bases;
    std::vector<size_t>       sources;
    for (size_t i = 0; i < support_layers.size(); ++i) {
        const SupportLayer& layer = *support_layers[i];
        // Final paths can have different heights even when their top Z is shared.
        std::map<float, std::pair<Polygons, bool>> by_height;
        auto                                       append = [&](const ExtrusionPath& path) {
            if (path.is_force_no_extrusion() || path.height <= 0.)
                return;
            auto& slab = by_height[path.height];
            path.polygons_covered_by_width(slab.first, SCALED_EPSILON);
            slab.second |= path.is_dynapin_landing;
        };
        const auto flattened = layer.support_fills.flatten();
        for (const ExtrusionEntity* entity : flattened.entities) {
            if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity))
                append(*path);
            else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(entity))
                for (const auto& path : multi->paths)
                    append(path);
            else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity))
                for (const auto& path : loop->paths)
                    append(path);
        }
        for (auto& [height, data] : by_height) {
            slabs.push_back({layer.print_z - height, layer.print_z, union_(std::move(data.first))});
            Polygons base;
            if (data.second) {
                base = layer.dynapin_landing_regions;
                // The inset controls adhesion extrusion, not the physical pin width.
                // Protect the full pin width while retaining this landing's X span.
                for (const auto& surface : surfaces) {
                    if (std::abs(layer.print_z - height - surface.print_z) > EPSILON)
                        continue;
                    for (const auto& contact : intersection(layer.dynapin_landing_regions, Polygons{surface.poly})) {
                        BoundingBox bounds     = get_extents(contact);
                        const auto  pin_bounds = get_extents(surface.poly);
                        bounds.min.y()         = pin_bounds.min.y();
                        bounds.max.y()         = pin_bounds.max.y();
                        polygons_append(base, intersection(Polygons{bounds.polygon()}, Polygons{surface.poly}));
                    }
                }
                base = union_(std::move(base));
            }
            bases.push_back(std::move(base));
            sources.push_back(i);
        }
    }
    auto regions = stabilization_regions(slabs, bases, layer_count);
    for (size_t i = 0; i < slabs.size(); ++i)
        polygons_append(result[sources[i]], std::move(regions[i]));
    for (Polygons& region : result)
        if (!region.empty())
            region = union_(std::move(region));
    return result;
}

std::vector<Polygons> landing_footprints(const std::vector<VirtualSupportSurface>& surfaces)
{
    std::vector<Polygons> result;
    for (const auto& surface : surfaces) {
        BoundingBox bounds = get_extents(surface.poly);
        for (const bool lower : {true, false}) {
            const bool adjacent = std::any_of(surfaces.begin(), surfaces.end(), [&](const auto& other) {
                if (other.pin.row != surface.pin.row || other.pin.col != surface.pin.col + (lower ? -1 : 1) ||
                    std::abs(other.print_z - surface.print_z) > EPSILON)
                    return false;
                const auto neighbor = get_extents(other.poly);
                return neighbor.min.x() < bounds.max.x() && neighbor.max.x() > bounds.min.x() &&
                       std::abs(double(lower ? neighbor.max.y() - bounds.min.y() : neighbor.min.y() - bounds.max.y())) <= SCALED_EPSILON;
            });
            if (!adjacent) {
                if (lower)
                    bounds.min.y() += scale_(surface.edge_inset_y);
                else
                    bounds.max.y() -= scale_(surface.edge_inset_y);
            }
        }
        if (bounds.min.y() >= bounds.max.y()) {
            result.emplace_back();
            continue;
        }
        result.push_back(intersection(Polygons{surface.poly}, Polygons{bounds.polygon()}));
    }
    return result;
}

std::vector<Polygons> independent_support_removals(const std::vector<LandingLayer>&          layers,
                                                   const std::vector<VirtualSupportSurface>& surfaces,
                                                   const std::vector<Polygons>&              targets)
{ return independent_support_removals(layers, surfaces, targets, {}); }

std::vector<Polygons> independent_support_removals(const std::vector<LandingLayer>&          layers,
                                                   const std::vector<VirtualSupportSurface>& surfaces,
                                                   const std::vector<Polygons>&              targets,
                                                   const std::vector<double>&                extrusion_margins)
{
    assert(layers.size() == targets.size());
    assert(extrusion_margins.empty() || extrusion_margins.size() == layers.size());
    std::vector<Polygons> result(layers.size());
    if (surfaces.empty())
        return result;
    const auto footprints = landing_footprints(surfaces);
    auto       root       = [](std::vector<size_t>& parents, size_t i) {
        while (parents[i] != i) {
            parents[i] = parents[parents[i]];
            i          = parents[i];
        }
        return i;
    };
    std::vector<size_t> pin_groups(surfaces.size());
    std::iota(pin_groups.begin(), pin_groups.end(), 0);
    for (size_t i = 0; i < surfaces.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (surfaces[i].pin == surfaces[j].pin ||
                (std::abs(surfaces[i].print_z - surfaces[j].print_z) < EPSILON && surfaces[i].pin.row == surfaces[j].pin.row &&
                 std::abs(surfaces[i].pin.col - surfaces[j].pin.col) == 1 &&
                 !intersection(offset({surfaces[i].poly}, float(SCALED_EPSILON)), {surfaces[j].poly}).empty()))
                pin_groups[root(pin_groups, i)] = root(pin_groups, j);
    std::vector<Polygons> limits(surfaces.size());
    for (size_t i = 0; i < surfaces.size(); ++i)
        polygons_append(limits[root(pin_groups, i)], footprints[i]);
    for (auto& limit : limits)
        limit = union_(limit);

    // Treat shared boundaries as connections as well. Only a coordinate-scale
    // tolerance is allowed here; an extrusion-width dilation could join islands.
    const auto connected = [](const Polygons& a, const Polygons& b) { return !intersection(offset(a, float(SCALED_EPSILON)), b).empty(); };
    struct Node
    {
        size_t   source;
        Polygons polygons;
        Polygons target;
        bool     lower = false;
        bool     upper = false;
    };
    std::vector<Node> nodes;
    for (size_t i = 0; i < layers.size(); ++i)
        for (const auto& island : union_ex(layers[i].polygons)) {
            Node node{i, to_polygons(island), {}};
            for (const auto& target : union_ex(targets[i]))
                if (!intersection(node.polygons, to_polygons(target)).empty())
                    polygons_append(node.target, to_polygons(target));
            nodes.push_back(std::move(node));
        }
    std::vector<size_t> parents(nodes.size());
    std::iota(parents.begin(), parents.end(), 0);
    std::vector<std::pair<size_t, size_t>> edges;
    std::vector<size_t>                    order(nodes.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return layers[nodes[a].source].bottom_z < layers[nodes[b].source].bottom_z; });
    for (size_t a = 0; a < order.size(); ++a) {
        const size_t i     = order[a];
        const auto&  first = layers[nodes[i].source];
        for (size_t b = a + 1; b < order.size(); ++b) {
            const size_t j      = order[b];
            const auto&  second = layers[nodes[j].source];
            if (second.bottom_z > first.print_z + EPSILON)
                break;
            if (!get_extents(nodes[i].polygons).overlap(get_extents(nodes[j].polygons)) || !connected(nodes[i].polygons, nodes[j].polygons))
                continue;
            parents[root(parents, i)] = root(parents, j);
            edges.emplace_back(i, j);
            // Base/interface slabs may overlap vertically before the native
            // generator modulates their heights. Follow their actual extents.
            if (first.bottom_z < second.bottom_z - EPSILON)
                nodes[j].lower = true;
            if (second.bottom_z < first.bottom_z - EPSILON)
                nodes[i].lower = true;
            if (first.print_z < second.print_z - EPSILON)
                nodes[i].upper = true;
            if (second.print_z < first.print_z - EPSILON)
                nodes[j].upper = true;
        }
    }
    std::vector<std::vector<size_t>> components(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        components[root(parents, i)].push_back(i);
    std::vector<Polygons> clipped(nodes.size());
    for (const auto& component : components) {
        if (component.empty())
            continue;
        bool             safe = true;
        std::set<size_t> landings;
        for (size_t i : component) {
            const auto& node = nodes[i];
            const auto& slab = layers[node.source];
            if (!node.lower) {
                bool on_pin = false;
                for (size_t j = 0; j < surfaces.size(); ++j)
                    if (std::abs(slab.bottom_z - surfaces[j].print_z) < EPSILON && !intersection(node.polygons, footprints[j]).empty()) {
                        on_pin = true;
                        landings.insert(root(pin_groups, j));
                    }
                safe &= on_pin;
            }
            // Source masks describe the complete contact, including islands that
            // only carry interface margin. An absent source mask is unknown.
            if (!node.upper && targets[node.source].empty())
                safe = false;
        }
        if (!safe || landings.size() != 1)
            continue;
        const auto& limit = limits[*landings.begin()];
        for (size_t i : component) {
            const auto&    node      = nodes[i];
            const Polygons printable = extrusion_margins.empty() ? limit : offset(limit, float(-scale_(extrusion_margins[node.source])));
            clipped[i]               = intersection(node.polygons, printable);
            // Preserve the complete associated target, including portions
            // beyond the support island (e.g. tree contact-point sampling).
            if (!node.target.empty() && !diff(node.target, printable).empty())
                safe = false;
            if (union_ex(clipped[i]).size() > 1)
                safe = false;
        }
        // A redundant edge may disappear without disconnecting the column.
        // Check the remaining component, rather than requiring every old edge.
        std::vector<size_t> clipped_parents(nodes.size());
        std::iota(clipped_parents.begin(), clipped_parents.end(), 0);
        for (const auto& edge : edges)
            if (root(parents, edge.first) == root(parents, component.front()) && !clipped[edge.first].empty() &&
                !clipped[edge.second].empty() && connected(clipped[edge.first], clipped[edge.second]))
                clipped_parents[root(clipped_parents, edge.first)] = root(clipped_parents, edge.second);
        std::set<size_t> remaining;
        for (size_t i : component)
            if (!clipped[i].empty())
                remaining.insert(root(clipped_parents, i));
        safe &= remaining.size() == 1;
        if (!safe)
            continue;
        for (size_t i : component)
            polygons_append(result[nodes[i].source], diff(nodes[i].polygons, clipped[i]));
    }
    return result;
}

std::vector<LandingLayer> landing_base_layers(const std::vector<LandingLayer>&          layers,
                                              const std::vector<VirtualSupportSurface>& surfaces,
                                              double                                    expansion)
{
    struct Footprint
    {
        double   print_z;
        Polygons polygons;
        Polygons source_polygons;
    };
    std::vector<Footprint> footprints;
    const auto             printable = landing_footprints(surfaces);
    for (size_t i = 0; i < surfaces.size(); ++i) {
        const auto& surface   = surfaces[i];
        const auto& footprint = printable[i];
        if (footprint.empty())
            continue;
        auto           match     = std::find_if(footprints.begin(), footprints.end(),
                                                [&](const auto& other) { return std::abs(other.print_z - surface.print_z) < EPSILON; });
        if (match == footprints.end())
            footprints.push_back({surface.print_z, footprint, {surface.poly}});
        else {
            polygons_append(match->polygons, footprint);
            match->source_polygons.push_back(surface.poly);
        }
    }
    std::vector<LandingLayer> result;
    for (auto& surface : footprints) {
        surface.polygons        = union_(surface.polygons);
        surface.source_polygons = union_(surface.source_polygons);
        std::vector<LandingLayer> groups;
        for (const auto& layer : layers) {
            if (std::abs(layer.bottom_z - surface.print_z) > EPSILON)
                continue;
            Polygons seeds = intersection(layer.polygons, surface.source_polygons);
            if (seeds.empty())
                continue;
            auto match = std::find_if(groups.begin(), groups.end(),
                                      [&](const auto& group) { return std::abs(group.print_z - layer.print_z) < EPSILON; });
            if (match == groups.end())
                groups.push_back({surface.print_z, layer.print_z, std::move(seeds)});
            else
                match->polygons = union_(match->polygons, seeds);
        }
        std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.print_z < b.print_z; });
        Polygons seeds_assigned;
        for (auto& group : groups) {
            group.polygons = diff(union_(group.polygons), seeds_assigned);
            polygons_append(seeds_assigned, group.polygons);
        }
        Polygons expanded_assigned;
        for (size_t i = 0; i < groups.size(); ++i) {
            Polygons polygons = intersection(offset(groups[i].polygons, scale_(expansion)), surface.polygons);
            // Different branches on one pin may have different first-layer
            // heights. Expansion must not invade another branch's first slab.
            for (size_t j = 0; j < groups.size(); ++j)
                if (i != j)
                    polygons = diff(polygons, groups[j].polygons);
            polygons = diff(polygons, expanded_assigned);
            polygons_append(expanded_assigned, polygons);
            if (polygons.empty())
                continue;
            auto match = std::find_if(result.begin(), result.end(), [&](const auto& layer) {
                return std::abs(layer.bottom_z - surface.print_z) < EPSILON && std::abs(layer.print_z - groups[i].print_z) < EPSILON;
            });
            if (match == result.end())
                result.push_back({surface.print_z, groups[i].print_z, std::move(polygons)});
            else
                match->polygons = union_(match->polygons, polygons);
        }
    }
    return result;
}

Polygons landing_printable_region(const PrintObject& object)
{
    Polygon     border = get_bed_shape_with_excluded_area(object.print()->config());
    const Point shift  = object.instances().empty() ? Point(0, 0) : object.instances().front().shift_without_plate_offset();
    border.translate(-shift);
    return {std::move(border)};
}

static ExtrusionPaths split_landing_path(const ExtrusionPath& path, const Polygons& regions)
{
    if (intersection_pl(path.polyline, regions).empty())
        return {path};
    if (diff_pl(path.polyline, regions).empty()) {
        ExtrusionPath landing(path);
        landing.is_dynapin_landing = true;
        return {std::move(landing)};
    }

    // Clip each segment to retain the original traversal order, including paths
    // which enter the same landing region more than once.
    ExtrusionPaths result;
    for (size_t i = 1; i < path.polyline.points.size(); ++i) {
        const Point&                           start = path.polyline.points[i - 1];
        Polyline                               segment(Points{start, path.polyline.points[i]});
        std::vector<std::pair<Polyline, bool>> fragments;
        for (Polyline& fragment : intersection_pl(segment, regions))
            fragments.emplace_back(std::move(fragment), true);
        for (Polyline& fragment : diff_pl(segment, regions))
            fragments.emplace_back(std::move(fragment), false);
        auto distance = [&](const Point& point) { return (point - start).cast<double>().squaredNorm(); };
        for (auto& fragment : fragments)
            if (distance(fragment.first.last_point()) < distance(fragment.first.first_point()))
                fragment.first.reverse();
        std::sort(fragments.begin(), fragments.end(),
                  [&](const auto& a, const auto& b) { return distance(a.first.first_point()) < distance(b.first.first_point()); });
        for (auto& [polyline, landing] : fragments) {
            if (!result.empty() && result.back().is_dynapin_landing == landing && result.back().last_point() == polyline.first_point())
                append(result.back().polyline.points, Points(polyline.points.begin() + 1, polyline.points.end()));
            else {
                result.emplace_back(std::move(polyline), path);
                result.back().is_dynapin_landing = landing;
            }
        }
    }
    return result;
}

void mark_landing_paths(ExtrusionEntityCollection& collection, const Polygons& regions)
{
    if (regions.empty())
        return;
    for (ExtrusionEntity*& entity : collection.entities) {
        if (auto* nested = dynamic_cast<ExtrusionEntityCollection*>(entity))
            mark_landing_paths(*nested, regions);
        else if (auto* path = dynamic_cast<ExtrusionPath*>(entity)) {
            ExtrusionPaths paths = split_landing_path(*path, regions);
            if (paths.size() == 1)
                *path = std::move(paths.front());
            else {
                auto* split  = new ExtrusionMultiPath();
                split->paths = std::move(paths);
                if (!path->can_reverse())
                    split->set_reverse();
                delete entity;
                entity = split;
            }
        } else {
            ExtrusionPaths* paths = nullptr;
            if (auto* multipath = dynamic_cast<ExtrusionMultiPath*>(entity))
                paths = &multipath->paths;
            else if (auto* loop = dynamic_cast<ExtrusionLoop*>(entity))
                paths = &loop->paths;
            if (paths != nullptr) {
                ExtrusionPaths split;
                for (const ExtrusionPath& path : *paths)
                    append(split, split_landing_path(path, regions));
                *paths = std::move(split);
            }
        }
    }
}

static double number_or(const nlohmann::json& j, const char* key, double fallback)
{
    if (!j.contains(key))
        return fallback;
    const nlohmann::json& value = j[key];
    if (value.is_number())
        return value.get<double>();
    if (value.is_string()) {
        try {
            return std::stod(value.get<std::string>());
        } catch (...) {}
    }
    return fallback;
}

static int int_or(const nlohmann::json& j, const char* key, int fallback)
{
    if (!j.contains(key))
        return fallback;
    const nlohmann::json& value = j[key];
    if (value.is_number_integer())
        return value.get<int>();
    if (value.is_string()) {
        try {
            return std::stoi(value.get<std::string>());
        } catch (...) {}
    }
    return fallback;
}

// The physical blocker/pin landing region is shifted along Y from the support
// origin. Pull G-code keeps its own origin and is intentionally independent of
// this support geometry.
static constexpr double support_block_y_offset = -7.2;

double pin_y(const Config& config, const Pin& pin) { return config.support_origin_y + double(pin.col) * config.col_pitch_y; }

double support_x_front(const Config& config)
{
    const PullConfig& pull = config.pull_gcode;
    switch (pull.type) {
    case PullType::NormalOnly: return pull.normal.x_front;
    case PullType::RevertedOnly: return pull.reverted.x_front;
    case PullType::Alternating: return std::min(pull.alternating.normal.x_front, pull.alternating.reverted.x_front);
    }
    return pull.normal.x_front;
}

BlockerZRange blocker_z_range(const Config& config, const Pin& pin)
{
    const double z_max = config.support_origin_z + double(pin.row) * config.row_pitch_z;
    return {z_max - config.blocker_height_z, z_max};
}

int effective_debug_stage(const Print& print)
{
    int stage = print.config().dynapin_debug_stage.value;
    if (const char* override_stage = std::getenv("DYNAPIN_DEBUG_STAGE")) {
        try {
            stage = std::stoi(override_stage);
        } catch (...) {
            // Keep the configured value when an environment override is not an
            // integer.  This mirrors the permissive handling of other debug
            // configuration values.
        }
    }
    return std::max(0, std::min(2, stage));
}

static const PullMoveConfig& pull_parameters(const Config& config, const Pin& pin)
{
    const PullConfig& pull = config.pull_gcode;
    switch (pull.type) {
    case PullType::NormalOnly: return pull.normal;
    case PullType::RevertedOnly: return pull.reverted;
    case PullType::Alternating:
        return pin.row % 2 == pull.alternating.reverted_row_parity ? pull.alternating.reverted : pull.alternating.normal;
    }
    return pull.normal;
}

std::vector<Pin> parse_pin_list(const std::string& pins)
{
    std::vector<Pin>              out;
    std::set<std::pair<int, int>> seen;
    std::vector<std::string>      tokens;
    boost::split(tokens, pins, boost::is_any_of(";\n "), boost::token_compress_on);
    for (const std::string& token : tokens) {
        if (token.empty())
            continue;
        std::vector<std::string> pair;
        boost::split(pair, token, boost::is_any_of(","), boost::token_compress_on);
        if (pair.size() != 2)
            continue;
        try {
            Pin pin{std::stoi(pair[0]), std::stoi(pair[1])};
            if (seen.emplace(pin.row, pin.col).second)
                out.push_back(pin);
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "Ignoring invalid DynaPin pin token: " << token;
        }
    }
    return out;
}

bool has_manual_selection(const Print& print)
{
    const std::string& value = print.config().dynapin_selected_pins.value;
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); });
}

std::vector<Pin> candidate_pins(const Config& config)
{
    std::vector<Pin> out;
    if (config.row_count <= 0 || config.col_count <= 0)
        return out;
    out.reserve(size_t(config.row_count) * size_t(config.col_count));
    for (int row = 0; row < config.row_count; ++row)
        for (int col = 0; col < config.col_count; ++col)
            out.push_back({row, col});
    return out;
}

void sort_unique_pins(std::vector<Pin>& pins, const Config& config)
{
    std::sort(pins.begin(), pins.end(), [&config](const Pin& lhs, const Pin& rhs) {
        const double lhs_z = blocker_z_range(config, lhs).z_max;
        const double rhs_z = blocker_z_range(config, rhs).z_max;
        if (lhs_z != rhs_z)
            return lhs_z < rhs_z;
        if (lhs.row != rhs.row)
            return lhs.row < rhs.row;
        return lhs.col < rhs.col;
    });
    pins.erase(std::unique(pins.begin(), pins.end()), pins.end());
}

struct SelectionWork
{
    size_t intersections = 0;
    size_t differences   = 0;
    size_t offsets       = 0;
    size_t unions        = 0;
};

static ProjectionSelection select_from_projection_impl(std::vector<ProjectionEvent> events, SelectionWork* work)
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
    auto                intersects = [&](const Polygons& a, const Polygons& b) {
        if (work)
            ++work->intersections;
        return !intersection(a, b).empty();
    };
    for (const ProjectionEvent& event : events) {
        if (event.type == ProjectionEventType::Contact) {
            polygons_append(projection, event.polygons);
            if (work)
                ++work->unions;
            projection = union_(projection);
        } else if (event.type == ProjectionEventType::Model) {
            if (!projection.empty()) {
                if (work) {
                    ++work->differences;
                    ++work->offsets;
                }
                projection = diff(projection, offset(event.polygons, float(SCALED_EPSILON)));
            }
        } else if (!projection.empty() && intersects(projection, event.polygons)) {
            if (event.colliding)
                result.rejected_collisions.push_back(event.pin);
            else {
                result.selected.push_back(event.pin);
                // This is the downward-propagation termination point. A safe
                // upper pin consumes only the projection landing on its top.
                if (work)
                    ++work->differences;
                projection = diff(projection, event.polygons);
            }
        }
    }
    return result;
}

ProjectionSelection select_from_projection(std::vector<ProjectionEvent> events)
{ return select_from_projection_impl(std::move(events), nullptr); }

ProjectionSelection select_tree_pins(const PrintObject&      object,
                                     const Config&           config,
                                     const std::vector<Pin>& candidates,
                                     const std::vector<Pin>& colliding)
{
    using Clock                = std::chrono::steady_clock;
    const auto    begin        = Clock::now();
    const auto&   contacts     = object.dynapin_tree_contacts();
    const auto    geometry_end = Clock::now();
    SelectionWork work;
    auto          intersect = [&](const Polygons& a, const Polygons& b) {
        ++work.intersections;
        return intersection(a, b);
    };
    std::vector<ProjectionEvent> events = contacts;
    events.reserve(contacts.size() + candidates.size());
    std::vector<VirtualSupportSurface>    surfaces;
    std::vector<BoundingBox>              surface_bounds;
    std::map<std::pair<int, int>, size_t> indices;
    std::vector<unsigned char>            is_colliding(candidates.size(), 0), is_selected(candidates.size(), 0);
    surfaces.reserve(candidates.size());
    surface_bounds.reserve(candidates.size());
    for (const Pin& pin : candidates) {
        const size_t index = surfaces.size();
        indices.emplace(std::make_pair(pin.row, pin.col), index);
        surfaces.push_back(surface_for_pin(object, config, pin));
        surface_bounds.push_back(get_extents(surfaces.back().poly));
        is_colliding[index]                  = std::find(colliding.begin(), colliding.end(), pin) != colliding.end();
        const VirtualSupportSurface& surface = surfaces.back();
        events.push_back({ProjectionEventType::PinSurface, surface.print_z, {surface.poly}, pin, bool(is_colliding[index])});
    }
    const auto          surfaces_end   = Clock::now();
    ProjectionSelection selection      = select_from_projection_impl(std::move(events), &work);
    const auto          projection_end = Clock::now();
    std::vector<size_t> selected_indices;
    selected_indices.reserve(candidates.size());
    for (const Pin& pin : selection.selected) {
        const size_t index = indices.at({pin.row, pin.col});
        is_selected[index] = 1;
        selected_indices.push_back(index);
    }
    // Strict separation only: touching bounds still take the exact polygon path.
    const auto separated = [](const BoundingBox& a, const BoundingBox& b) {
        return a.max.x() < b.min.x() || b.max.x() < a.min.x() || a.max.y() < b.min.y() || b.max.y() < a.min.y();
    };
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
    std::vector<BoundingBox> contact_bounds;
    contact_bounds.reserve(contacts.size());
    for (const auto& contact : contacts)
        contact_bounds.push_back(get_extents(contact.polygons));
    std::optional<double>                reach_height;
    std::vector<std::optional<Polygons>> contact_reach(contacts.size());
    for (const Pin& pin : nearby_candidates) {
        const size_t pin_index = indices.at({pin.row, pin.col});
        if (is_colliding[pin_index] || is_selected[pin_index])
            continue;
        const VirtualSupportSurface& surface = surfaces[pin_index];
        // At an identical top height, newly added pins cannot enter the strict
        // upper-pin condition below. Remaining contact geometry and its exact
        // offset therefore stay unchanged throughout this height group.
        if (!reach_height || *reach_height != surface.print_z) {
            reach_height = surface.print_z;
            for (auto& reach : contact_reach)
                reach.reset();
        }
        for (size_t contact_index = 0; contact_index < contacts.size(); ++contact_index) {
            const ProjectionEvent& contact = contacts[contact_index];
            if (contact.print_z <= surface.print_z + EPSILON)
                continue;
            const bool have_reach = contact_reach[contact_index].has_value();
            Polygons   remaining  = have_reach ? Polygons{} : contact.polygons;
            bool       shadowed   = false;
            for (const size_t selected_index : selected_indices) {
                const VirtualSupportSurface& selected_surface = surfaces[selected_index];
                if (selected_surface.print_z > surface.print_z + EPSILON && selected_surface.print_z < contact.print_z - EPSILON) {
                    if (!separated(surface_bounds[selected_index], surface_bounds[pin_index]) &&
                        !separated(contact_bounds[contact_index], surface_bounds[selected_index]) &&
                        !intersect({selected_surface.poly}, {surface.poly}).empty() &&
                        !intersect(contact.polygons, {selected_surface.poly}).empty()) {
                        shadowed = true;
                        break;
                    }
                    if (!have_reach && !separated(get_extents(remaining), surface_bounds[selected_index])) {
                        ++work.differences;
                        remaining = diff(remaining, {selected_surface.poly});
                    }
                    if (!have_reach && remaining.empty())
                        break;
                }
            }
            if (shadowed || (!have_reach && remaining.empty()))
                continue;
            if (!have_reach) {
                const double travel = move_per_layer * std::ceil((contact.print_z - surface.print_z) / layer_height);
                ++work.offsets;
                contact_reach[contact_index] = offset(remaining, float(scale_(travel)));
            }
            if (!intersect(*contact_reach[contact_index], {surface.poly}).empty()) {
                selection.selected.push_back(pin);
                selected_indices.push_back(pin_index);
                is_selected[pin_index] = 1;
                break;
            }
        }
    }
    sort_unique_pins(selection.selected, config);
    const auto end = Clock::now();
    auto       ms  = [](auto duration) { return std::chrono::duration<double, std::milli>(duration).count(); };
    BOOST_LOG_TRIVIAL(info) << "[DynaPin] Tree pin selection: geometry_ms=" << ms(geometry_end - begin)
                            << ", surfaces_ms=" << ms(surfaces_end - geometry_end)
                            << ", projection_ms=" << ms(projection_end - surfaces_end) << ", nearby_ms=" << ms(end - projection_end)
                            << ", surfaces=" << surfaces.size() << ", intersections=" << work.intersections
                            << ", differences=" << work.differences << ", offsets=" << work.offsets << ", unions=" << work.unions;
    return selection;
}

const std::vector<Pin>& resolved_pins(const Print& print) { return print.dynapin_selection().pins; }

static std::vector<fs::path> config_candidates(const std::string& path)
{
    fs::path p(path);
    if (p.is_absolute())
        return {p};

    std::vector<fs::path> out;
    if (!data_dir().empty()) {
        out.emplace_back(fs::path(data_dir()) / "user" / p);
        out.emplace_back(fs::path(data_dir()) / "system" / p);
    }
    if (!resources_dir().empty()) {
        out.emplace_back(fs::path(resources_dir()) / "profiles" / p);
        out.emplace_back(fs::path(resources_dir()) / p);
    }
    out.emplace_back(p);
    return out;
}

static bool has_number_value(const nlohmann::json& object, const char* key)
{
    if (!object.contains(key))
        return false;
    const nlohmann::json& value = object[key];
    if (value.is_number())
        return true;
    if (!value.is_string())
        return false;

    try {
        size_t consumed = 0;
        std::stod(value.get<std::string>(), &consumed);
        return consumed == value.get<std::string>().size();
    } catch (...) {
        return false;
    }
}

static std::optional<double> bed_max_x(const Print& print)
{
    const Points bed = get_bed_shape(print.config());
    if (bed.empty())
        return std::nullopt;

    coord_t max_x = bed.front().x();
    for (const Point& point : bed)
        max_x = std::max(max_x, point.x());
    return unscale_(max_x);
}

bool load_config_for_print(const Print& print, Config& config, std::string* error)
{
    const std::string config_path = print.config().dynapin_config_path.value;
    if (config_path.empty()) {
        if (error)
            *error = "DynaPin config path is empty";
        return false;
    }

    fs::path resolved;
    for (const fs::path& candidate : config_candidates(config_path)) {
        if (fs::exists(candidate)) {
            resolved = candidate;
            break;
        }
    }
    if (resolved.empty()) {
        if (error)
            *error = "DynaPin config file not found: " + config_path;
        return false;
    }

    boost::nowide::ifstream ifs(resolved.string());
    if (!ifs) {
        if (error)
            *error = "Failed to open DynaPin config: " + resolved.string();
        return false;
    }

    nlohmann::json j = nlohmann::json::parse(ifs, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (error)
            *error = "Invalid DynaPin JSON: " + resolved.string();
        return false;
    }

    const nlohmann::json grid           = j.contains("grid") && j["grid"].is_object() ? j["grid"] : nlohmann::json::object();
    config.row_count                    = int_or(grid, "row_count", config.row_count);
    config.col_count                    = int_or(grid, "col_count", config.col_count);
    const nlohmann::json pull_origin    = grid.contains("pull_origin") && grid["pull_origin"].is_object() ? grid["pull_origin"] :
                                                                                                            nlohmann::json::object();
    config.pull_origin_y                = number_or(pull_origin, "y", config.pull_origin_y);
    config.pull_origin_z                = number_or(pull_origin, "z", config.pull_origin_z);
    const nlohmann::json support_origin = grid.contains("support_origin") && grid["support_origin"].is_object() ? grid["support_origin"] :
                                                                                                                  nlohmann::json::object();
    config.support_origin_y             = number_or(support_origin, "y", config.support_origin_y);
    config.support_origin_z             = number_or(support_origin, "z", config.support_origin_z);
    const nlohmann::json pitch          = grid.contains("pitch") && grid["pitch"].is_object() ? grid["pitch"] : nlohmann::json::object();
    config.row_pitch_z                  = number_or(pitch, "row_z", config.row_pitch_z);
    config.col_pitch_y                  = number_or(pitch, "col_y", config.col_pitch_y);

    const nlohmann::json exclusion = j.contains("support_exclusion") && j["support_exclusion"].is_object() ? j["support_exclusion"] :
                                                                                                             nlohmann::json::object();
    config.blocker_width_y         = number_or(exclusion, "blocker_width_y", config.blocker_width_y);
    config.blocker_height_z        = number_or(exclusion, "blocker_height_z", config.blocker_height_z);
    config.landing_edge_inset_y    = number_or(exclusion, "landing_edge_inset_y", config.landing_edge_inset_y);

    const auto fail = [error](const std::string& message) {
        if (error)
            *error = message;
        return false;
    };
    config.landing_slope = {};
    if (j.contains("landing_slope")) {
        const auto& settings = j["landing_slope"];
        if (!settings.is_object())
            return fail("DynaPin landing_slope must be an object");
        for (const auto& field : {std::pair<const char*, double*>{"reference_x", &config.landing_slope.reference_x},
                                  {"slope_x", &config.landing_slope.slope_x},
                                  {"max_band_width", &config.landing_slope.max_band_width},
                                  {"travel_clearance", &config.landing_slope.travel_clearance}}) {
            if (!settings.contains(field.first))
                continue;
            if (!has_number_value(settings, field.first))
                return fail(std::string("DynaPin landing_slope.") + field.first + " must be a finite number");
            *field.second = number_or(settings, field.first, 0.);
            if (!std::isfinite(*field.second))
                return fail(std::string("DynaPin landing_slope.") + field.first + " must be a finite number");
        }
        if (config.landing_slope.max_band_width < unscale<double>(coord_t(1)) || config.landing_slope.travel_clearance <= 0.)
            return fail("DynaPin landing_slope requires positive max_band_width and travel_clearance");
    }
    if (j.contains("support_speed")) {
        const auto& settings = j["support_speed"];
        if (!settings.is_object())
            return fail("DynaPin support_speed must be an object");
        if (settings.contains("layers")) {
            if (!settings["layers"].is_number_integer() || settings["layers"].get<double>() < 0. ||
                settings["layers"].get<double>() > double(std::numeric_limits<int>::max()))
                return fail("DynaPin support_speed.layers must be a non-negative integer");
            config.support_speed.layers = settings["layers"].get<int>();
        }
        // Legacy min_x_distance is ignored: short segments also need protection.
        if (settings.contains("max_x_speed")) {
            if (!settings["max_x_speed"].is_number())
                return fail("DynaPin support_speed.max_x_speed must be a number");
            config.support_speed.max_x_speed = settings["max_x_speed"].get<double>();
        }
        if (!std::isfinite(config.support_speed.max_x_speed) || config.support_speed.max_x_speed < 0.)
            return fail("DynaPin support_speed.max_x_speed must be finite and non-negative");
    }
    const nlohmann::json pull = j.value("pull_gcode", nlohmann::json::object());
    if (!pull.is_object() || !pull.contains("type") || !pull["type"].is_string() || !pull.contains("normal") ||
        !pull["normal"].is_object() || !pull.contains("alternating") || !pull["alternating"].is_object() || !pull.contains("reverted") ||
        !pull["reverted"].is_object())
        return fail("DynaPin requires pull_gcode.type, normal, alternating and reverted (legacy JSON is unsupported)");
    const std::string type = pull["type"].get<std::string>();
    if (type == "normal")
        config.pull_gcode.type = PullType::NormalOnly;
    else if (type == "reverted")
        config.pull_gcode.type = PullType::RevertedOnly;
    else if (type == "alternating")
        config.pull_gcode.type = PullType::Alternating;
    else
        return fail("Invalid DynaPin pull_gcode.type: " + type);

    const auto read_numbers = [&](const nlohmann::json& object, const std::string& prefix,
                                  std::initializer_list<std::pair<const char*, double*>> fields) {
        for (const auto& field : fields) {
            if (!has_number_value(object, field.first))
                return fail(prefix + "." + field.first + " must be a finite number");
            const double value = number_or(object, field.first, 0.);
            if (!std::isfinite(value))
                return fail(prefix + "." + field.first + " must be a finite number");
            *field.second = value;
        }
        return true;
    };
    const auto read_profile = [&](const nlohmann::json& object, PullMoveConfig& profile, const std::string& prefix, bool alternating) {
        if (!read_numbers(object, prefix,
                          {{"x_hook", &profile.x_hook},
                           {"x_latch", &profile.x_latch},
                           {"x_front", &profile.x_front},
                           {"fast_feed_rate", &profile.fast_feed_rate},
                           {"feed_rate", &profile.feed_rate},
                           {"pull_feedrate_fast", &profile.pull_feedrate_fast},
                           {"y_origin_offset", &profile.y_origin_offset},
                           {"y_offset", &profile.y_offset},
                           {"approach_y_offset", &profile.approach_y_offset},
                           {"approach_z_offset", &profile.approach_z_offset},
                           {"engage_z_drop", &profile.engage_z_drop},
                           {"disengage_x_offset", &profile.disengage_x_offset},
                           {"disengage_z_lift", &profile.disengage_z_lift},
                           {"retreat_z_lift", &profile.retreat_z_lift}}))
            return false;
        if (alternating && !read_numbers(object, prefix, {{"alternating_y_offset", &profile.alternating_y_offset}}))
            return false;
        if (profile.fast_feed_rate <= 0. || profile.feed_rate <= 0. || profile.pull_feedrate_fast <= 0.)
            return fail(prefix + " feed rates must be positive");
        return true;
    };
    if (!read_profile(pull["normal"], config.pull_gcode.normal, "pull_gcode.normal", false) ||
        !read_profile(pull["reverted"], config.pull_gcode.reverted, "pull_gcode.reverted", false))
        return false;

    const nlohmann::json& alternating = pull["alternating"];
    if (!alternating.contains("row_parity") || !alternating["row_parity"].is_number_integer() ||
        (alternating["row_parity"] != 0 && alternating["row_parity"] != 1))
        return fail("DynaPin pull_gcode.alternating.row_parity must be 0 or 1");
    config.pull_gcode.alternating.reverted_row_parity = alternating["row_parity"].get<int>();
    if (!alternating.contains("normal") || !alternating["normal"].is_object() || !alternating.contains("reverted") ||
        !alternating["reverted"].is_object())
        return fail("DynaPin pull_gcode.alternating requires normal and reverted profiles");
    if (!read_profile(alternating["normal"], config.pull_gcode.alternating.normal, "pull_gcode.alternating.normal", true) ||
        !read_profile(alternating["reverted"], config.pull_gcode.alternating.reverted, "pull_gcode.alternating.reverted", true))
        return false;

    const std::optional<double> max_x_bed = bed_max_x(print);
    if (!max_x_bed) {
        const std::string message = "DynaPin config requires a printable bed X range";
        if (error)
            *error = message;
        else
            BOOST_LOG_TRIVIAL(warning) << "[DynaPin] cannot generate geometry: " << message;
        return false;
    }
    const auto valid_x_front = [&max_x_bed](double x_front) { return std::isfinite(x_front) && x_front < *max_x_bed; };
    if (!valid_x_front(config.pull_gcode.normal.x_front) || !valid_x_front(config.pull_gcode.reverted.x_front) ||
        !valid_x_front(config.pull_gcode.alternating.normal.x_front) || !valid_x_front(config.pull_gcode.alternating.reverted.x_front) ||
        config.blocker_width_y <= 0. || config.row_pitch_z == 0. || config.col_pitch_y == 0. ||
        !std::isfinite(config.landing_edge_inset_y) || config.landing_edge_inset_y < 0. ||
        2. * config.landing_edge_inset_y >= config.blocker_width_y) {
        if (error)
            *error = "DynaPin config requires valid pull_gcode x_front values and grid/support_exclusion dimensions";
        return false;
    }

    if (config.landing_slope.slope_x != 0.) {
        const double span = *max_x_bed - support_x_front(config);
        const double a    = pin_top_z_at_x(config, {0, 0}, support_x_front(config));
        const double b    = pin_top_z_at_x(config, {std::max(0, config.row_count - 1), 0}, *max_x_bed);
        if (span / config.landing_slope.max_band_width > 10000. ||
            config.landing_slope.max_band_width > unscale<double>(std::numeric_limits<coord_t>::max() / 2) || !std::isfinite(a) ||
            !std::isfinite(b))
            return fail("DynaPin landing_slope requires finite endpoint heights and at most 10000 bands per pin");
    }
    const BlockerZRange zero_row_range = blocker_z_range(config, {0, 0});
    BOOST_LOG_TRIVIAL(debug) << "[DynaPin] config loaded: requested='" << config_path << "', resolved='" << resolved.string()
                             << "', grid_size=" << config.row_count << "x" << config.col_count << ", pull_origin_mm=("
                             << config.pull_origin_y << "," << config.pull_origin_z << ")"
                             << ", support_origin_mm=(" << config.support_origin_y << "," << config.support_origin_z << ")"
                             << ", pitch_mm=(row_z=" << config.row_pitch_z << ", col_y=" << config.col_pitch_y << ")"
                             << ", exclusion_x_mm=[" << support_x_front(config) << "," << *max_x_bed << "]"
                             << ", exclusion_y_center_mm=" << config.support_origin_y + support_block_y_offset
                             << ", exclusion_width_y_mm=" << config.blocker_width_y << ", zero_row_blocker_z_range_mm=["
                             << zero_row_range.z_min << "," << zero_row_range.z_max << "]";
    return true;
}

static LocalBlocker blocker_for_pin_shift(const Config& config, const Pin& pin, const Point& shift, double max_x_bed)
{
    const double        y       = pin_y(config, pin);
    const BlockerZRange z_range = blocker_z_range(config, pin);
    const double        block_y = y + support_block_y_offset;
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

LocalBlocker blocker_for_pin(const PrintObject& object, const Config& config, const Pin& pin)
{
    const Print&                print = *object.print();
    const Point                 shift = object.instances().empty() ? Point(0, 0) : object.instances().front().shift_without_plate_offset();
    const std::optional<double> max_x_bed = bed_max_x(print);
    if (!max_x_bed)
        return {};
    return blocker_for_pin_shift(config, pin, shift, *max_x_bed);
}

static VirtualSupportSurface surface_for_pin_shift(const Config& config, const Pin& pin, const Point& shift, double max_x_bed)
{
    const double block_y = pin_y(config, pin) + support_block_y_offset;
    const double min_x   = support_x_front(config);
    const double max_x   = max_x_bed;
    const double min_y   = block_y - 0.5 * config.blocker_width_y;
    const double max_y   = block_y + 0.5 * config.blocker_width_y;

    VirtualSupportSurface surface;
    surface.pin         = pin;
    surface.print_z      = std::max(pin_top_z_at_x(config, pin, min_x), pin_top_z_at_x(config, pin, max_x));
    surface.edge_inset_y = config.landing_edge_inset_y;
    surface.poly.points = {Point(scale_(min_x) - shift.x(), scale_(min_y) - shift.y()),
                           Point(scale_(max_x) - shift.x(), scale_(min_y) - shift.y()),
                           Point(scale_(max_x) - shift.x(), scale_(max_y) - shift.y()),
                           Point(scale_(min_x) - shift.x(), scale_(max_y) - shift.y())};
    return surface;
}

VirtualSupportSurface surface_for_pin(const PrintObject& object, const Config& config, const Pin& pin)
{
    const Print&                print = *object.print();
    const Point                 shift = object.instances().empty() ? Point(0, 0) : object.instances().front().shift_without_plate_offset();
    const std::optional<double> max_x_bed = bed_max_x(print);
    if (!max_x_bed)
        return {};
    return surface_for_pin_shift(config, pin, shift, *max_x_bed);
}

double pin_top_z_at_x(const Config& config, const Pin& pin, double machine_x)
{ return blocker_z_range(config, pin).z_max + config.landing_slope.slope_x * (machine_x - config.landing_slope.reference_x); }

std::vector<VirtualSupportSurface> landing_surfaces_for_pin(const PrintObject& object, const Config& config, const Pin& pin)
{
    const auto envelope = surface_for_pin(object, config, pin);
    if (config.landing_slope.slope_x == 0. || envelope.poly.empty())
        return {envelope};
    const auto    bounds = get_extents(envelope.poly);
    const Point   shift  = object.instances().empty() ? Point(0, 0) : object.instances().front().shift_without_plate_offset();
    const coord_t width  = std::max<coord_t>(1, scale_(config.landing_slope.max_band_width));
    std::vector<VirtualSupportSurface> result;
    for (coord_t x = bounds.min.x(); x < bounds.max.x();) {
        const coord_t end   = std::min(x + width, bounds.max.x());
        auto          band  = envelope;
        auto          box   = bounds;
        box.min.x()         = x;
        box.max.x()         = end;
        band.poly           = box.polygon();
        band.source_print_z = envelope.print_z;
        band.print_z        = std::max(pin_top_z_at_x(config, pin, unscale<double>(x + shift.x())),
                                       pin_top_z_at_x(config, pin, unscale<double>(end + shift.x())));
        result.push_back(std::move(band));
        x = end;
    }
    return result;
}

std::vector<VirtualSupportSurface> landing_surfaces_for_object(const PrintObject& object)
{
    const auto envelopes = pin_top_surfaces_for_object(object);
    Config     config;
    if (envelopes.empty() || !load_config_for_print(*object.print(), config) || config.landing_slope.slope_x == 0.)
        return envelopes;
    std::vector<VirtualSupportSurface> result;
    for (const auto& envelope : envelopes) {
        auto bands = landing_surfaces_for_pin(object, config, envelope.pin);
        result.insert(result.end(), bands.begin(), bands.end());
    }
    return result;
}

double pin_pull_trigger_z(const Print& print, const Config& config, const Pin& pin)
{
    double result = blocker_z_range(config, pin).z_max;
    if (config.landing_slope.slope_x == 0.)
        return result;
    // Pull before the earliest physical top; an initial extrusion is always
    // above it. Do not depend on per-object layer indices or merged heights.
    if (const auto max_x = bed_max_x(print))
        result = std::min(pin_top_z_at_x(config, pin, support_x_front(config)), pin_top_z_at_x(config, pin, *max_x));
    return result;
}

double pin_travel_clearance_z(const Print& print, const Config& config, const std::vector<Pin>& pins)
{
    double result = 0.;
    if (const auto max_x = bed_max_x(print))
        for (const auto& pin : pins)
            result = std::max(result, std::max(pin_top_z_at_x(config, pin, support_x_front(config)), pin_top_z_at_x(config, pin, *max_x)) +
                                          config.landing_slope.travel_clearance);
    return result;
}

std::optional<Point> nearest_pin_landing_point(const Polygon& safe_region, const Point& from)
{
    if (safe_region.contains(from))
        return from;
    std::optional<Point> closest;
    double best_distance2 = std::numeric_limits<double>::max();
    for (const Line& edge : safe_region.lines()) {
        const Vec2d direction = (edge.b - edge.a).cast<double>();
        const double length2 = direction.squaredNorm();
        if (length2 <= 0.)
            continue;
        const double edge_margin = std::min(0.25, double(scale_(0.001)) / std::sqrt(length2));
        const double fraction = std::clamp((from - edge.a).cast<double>().dot(direction) / length2,
                                           edge_margin, 1. - edge_margin);
        const Point candidate = edge.a + Point(coord_t(std::llround(direction.x() * fraction)),
                                               coord_t(std::llround(direction.y() * fraction)));
        const double distance2 = (candidate - from).cast<double>().squaredNorm();
        if (distance2 < best_distance2) {
            best_distance2 = distance2;
            closest = candidate;
        }
    }
    return closest;
}

double required_pin_travel_z(const Print&            print,
                             const Config&           config,
                             const std::vector<Pin>& pulled_pins,
                             const Polyline&         local_path,
                             const Vec2d&            machine_origin,
                             double                  target_z,
                             double                  nozzle_radius)
{
    const auto max_x = bed_max_x(print);
    if (!max_x || local_path.size() < 2)
        return 0.;
    Polyline machine_path = local_path;
    machine_path.translate(scaled<coord_t>(machine_origin));
    const double min_x     = support_x_front(config);
    double       clearance = 0.;
    for (const Pin& pin : pulled_pins) {
        const double      y = pin_y(config, pin) + support_block_y_offset;
        const BoundingBox bounds(Point::new_scale(min_x - nozzle_radius, y - 0.5 * config.blocker_width_y - nozzle_radius),
                                 Point::new_scale(*max_x + nozzle_radius, y + 0.5 * config.blocker_width_y + nozzle_radius));
        const auto        intersections = intersection_pl(machine_path, Polygons{bounds.polygon()});
        if (intersections.empty())
            continue;
        double highest = std::numeric_limits<double>::lowest();
        for (const auto& segment : intersections)
            for (const auto& point : segment.points) {
                const double x = unscale<double>(point.x());
                for (double edge_x : {x - nozzle_radius, x + nozzle_radius})
                    highest = std::max(highest, pin_top_z_at_x(config, pin, std::clamp(edge_x, min_x, *max_x)));
            }
        // travel_clearance is the amount to lift when crossing an obstacle,
        // not a minimum gap to enforce above every first-layer scan line.
        if (highest >= target_z - EPSILON)
            clearance = std::max(clearance, highest + config.landing_slope.travel_clearance);
    }
    return clearance;
}

bool pin_collides_with_model(const Print& print, const Config& config, const Pin& pin, std::size_t* intersection_count)
{
    const std::optional<double> max_x_bed = bed_max_x(print);
    if (!max_x_bed) {
        BOOST_LOG_TRIVIAL(warning) << "[DynaPin] cannot check pin collision without a printable bed X range";
        return true;
    }
    for (const PrintObject* object : print.objects()) {
        const auto check_instance = [&](const Point& shift) {
            LocalBlocker blocker = blocker_for_pin_shift(config, pin, shift, *max_x_bed);
            if (config.landing_slope.slope_x != 0.) {
                const double a = pin_top_z_at_x(config, pin, support_x_front(config));
                const double b = pin_top_z_at_x(config, pin, *max_x_bed);
                // Selection uses a conservative envelope, including the lower
                // root. Actual support clipping uses the separate band prisms.
                blocker.z_min = std::min(a, b) - config.blocker_height_z;
                blocker.z_max = std::max(a, b);
            }
            const auto&        layers         = object->layers();
            const auto&        polygons       = object->dynapin_layer_polygons();
            const auto         first          = std::lower_bound(layers.begin(), layers.end(), blocker.z_min,
                                                                 [](const Layer* layer, double z) { return layer->print_z + EPSILON < z; });
            const auto         last           = std::upper_bound(first, layers.end(), blocker.z_max,
                                                                 [](double z, const Layer* layer) { return z < layer->print_z - EPSILON; });
            const BoundingBox  blocker_bounds = get_extents(blocker.poly);
            for (auto it = first; it != last; ++it) {
                const Layer*      layer          = *it;
                const auto&       layer_polygons = polygons[size_t(it - layers.begin())];
                const BoundingBox bounds         = get_extents(layer_polygons);
                if (bounds.max.x() < blocker_bounds.min.x() || blocker_bounds.max.x() < bounds.min.x() ||
                    bounds.max.y() < blocker_bounds.min.y() || blocker_bounds.max.y() < bounds.min.y())
                    continue;
                if (intersection_count)
                    ++*intersection_count;
                const Polygons overlap = intersection(layer_polygons, {blocker.poly});
                if (!overlap.empty()) {
                    BOOST_LOG_TRIVIAL(trace) << "[DynaPin] model collision: pin=(" << pin.row << "," << pin.col << ")"
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

std::vector<Polygons> support_blockers_for_object(const PrintObject& object)
{
    std::vector<Polygons> out(object.layer_count());
    const Print&          print = *object.print();
    if (!print.config().enable_dynapin_support_optimization.value || effective_debug_stage(print) < 2)
        return out;

    Config      config;
    std::string error;
    if (!load_config_for_print(print, config, &error)) {
        BOOST_LOG_TRIVIAL(warning) << error;
        return out;
    }

    for (const auto& blocker : support_blocker_regions_local(object))
        for (const Layer* layer : object.layers())
            if (layer->print_z + EPSILON >= blocker.z_min && layer->print_z - EPSILON <= blocker.z_max)
                out[layer->id()].push_back(blocker.poly);
    return out;
}

std::vector<LocalBlocker> support_blocker_regions_local(const PrintObject& object)
{
    std::vector<LocalBlocker> out;
    const Print&              print = *object.print();
    if (!print.config().enable_dynapin_support_optimization.value || effective_debug_stage(print) < 2)
        return out;

    Config      config;
    std::string error;
    if (!load_config_for_print(print, config, &error)) {
        BOOST_LOG_TRIVIAL(warning) << error;
        return out;
    }

    const std::vector<Pin>& pins = resolved_pins(print);
    out.reserve(pins.size());
    for (const Pin& pin : pins)
        for (const auto& surface : landing_surfaces_for_pin(object, config, pin))
            out.push_back({pin, surface.poly, surface.print_z - config.blocker_height_z, surface.print_z});
    return out;
}

std::vector<VirtualSupportSurface> pin_top_surfaces_for_object(const PrintObject& object)
{
    std::vector<VirtualSupportSurface> out;
    const Print&                       print = *object.print();
    if (!print.config().enable_dynapin_support_optimization.value || effective_debug_stage(print) < 1)
        return out;

    Config      config;
    std::string error;
    if (!load_config_for_print(print, config, &error)) {
        BOOST_LOG_TRIVIAL(warning) << error;
        return out;
    }

    const std::vector<Pin>& pins = resolved_pins(print);
    out.reserve(pins.size());
    for (const Pin& pin : pins)
        out.push_back(surface_for_pin(object, config, pin));
    return out;
}

std::vector<BlockerBox> selected_blocker_boxes(const Print& print)
{
    std::vector<BlockerBox> out;
    if (!print.config().enable_dynapin_support_optimization.value || effective_debug_stage(print) < 1)
        return out;

    Config      config;
    std::string error;
    if (!load_config_for_print(print, config, &error)) {
        BOOST_LOG_TRIVIAL(warning) << error;
        return out;
    }

    const std::vector<Pin>&     pins      = resolved_pins(print);
    const std::optional<double> max_x_bed = bed_max_x(print);
    if (!max_x_bed)
        return out;
    out.reserve(pins.size());
    for (const Pin& pin : pins) {
        const double        y       = pin_y(config, pin);
        const BlockerZRange z_range = blocker_z_range(config, pin);
        const double        block_y = y + support_block_y_offset;
        const double        min_x   = support_x_front(config);
        const double        max_x   = *max_x_bed;
        const double        min_y   = block_y - 0.5 * config.blocker_width_y;
        const double        max_y   = block_y + 0.5 * config.blocker_width_y;
        BlockerBox          box;
        box.pin     = pin;
        box.min     = Vec3d(min_x, min_y, z_range.z_min);
        box.max     = Vec3d(max_x, max_y, z_range.z_max);
        box.pin_pos = Vec3d(min_x, block_y, z_range.z_max);
        out.push_back(box);
    }
    return out;
}

std::string pull_gcode_for_pin(const Config& config, const Pin& pin)
{
    const PullMoveConfig& profile     = pull_parameters(config, pin);
    const bool            alternating = config.pull_gcode.type == PullType::Alternating;
    double                y = config.pull_origin_y + double(pin.col) * config.col_pitch_y + profile.y_offset + profile.y_origin_offset;
    if (alternating)
        y += profile.alternating_y_offset;
    const double z     = config.pull_origin_z + double(pin.row) * config.row_pitch_z;
    double       y_app = y + profile.approach_y_offset;
    if (config.pull_gcode.type == PullType::RevertedOnly)
        std::swap(y, y_app);
    const double       z_app    = z + (alternating ? profile.approach_z_offset : 0.);
    const double       z_engage = z_app - (alternating ? profile.engage_z_drop : 0.);
    std::ostringstream gcode;
    gcode << std::fixed << std::setprecision(4);
    gcode << "; BEGIN_DYNAPIN_PULL ROW=" << pin.row << " COL=" << pin.col << "\n";
    gcode << "G1 Y" << y_app << " F" << profile.fast_feed_rate << "\n";
    gcode << "G1 X" << profile.x_hook << " Z" << z_app << " F" << profile.fast_feed_rate << "\n";
    gcode << "G1 X" << profile.x_latch << " F" << profile.feed_rate << "\n";
    if (alternating)
        gcode << "G1 Z" << z_engage << " F" << profile.feed_rate << "\n";
    else
        gcode << "G1 Y" << y << " F" << profile.feed_rate << "\n";
    gcode << "; DYNAPIN_PULL_MOVE\n";
    gcode << "G1 X" << profile.x_front << " F" << profile.pull_feedrate_fast << "\n";
    gcode << "G1 X" << (profile.x_front + profile.disengage_x_offset);
    if (alternating)
        gcode << " Z" << (z_engage + profile.disengage_z_lift);
    else
        gcode << " Y" << y_app;
    gcode << " F" << profile.feed_rate << "\n";
    if (!alternating)
        gcode << "G1 Z" << (z + profile.retreat_z_lift) << " F" << profile.feed_rate << "\n";
    gcode << "; END_DYNAPIN_PULL\n";
    return gcode.str();
}

std::string return_gcode(const Config& config, const Vec3d& return_position, double clearance_z)
{
    const PullMoveConfig& profile = config.pull_gcode.type == PullType::Alternating  ? config.pull_gcode.alternating.normal :
                                    config.pull_gcode.type == PullType::RevertedOnly ? config.pull_gcode.reverted :
                                                                                       config.pull_gcode.normal;
    std::ostringstream    gcode;
    gcode << std::fixed << std::setprecision(4);
    gcode << "; BEGIN_DYNAPIN_RETURN\n";
    if (clearance_z > 0.)
        gcode << "G1 Z" << std::max(clearance_z, return_position.z()) << " F" << profile.fast_feed_rate << "\n";
    gcode << "G1 X" << return_position.x() << " Y" << return_position.y() << " F" << profile.fast_feed_rate << "\n";
    gcode << "G1 Z" << return_position.z() << " F" << profile.feed_rate << "\n";
    gcode << "; END_DYNAPIN_RETURN\n";
    return gcode.str();
}

} // namespace Slic3r::DynaPin
