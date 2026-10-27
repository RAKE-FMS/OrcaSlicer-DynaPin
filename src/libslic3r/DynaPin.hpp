#ifndef slic3r_DynaPin_hpp_
#define slic3r_DynaPin_hpp_

#include "ExPolygon.hpp"

#include <cstddef>
#include <string>
#include <optional>
#include <vector>

namespace Slic3r {

class Print;
class PrintObject;
class ExtrusionEntityCollection;

namespace DynaPin {
enum class SupportGeometryMode { Exact, PlacementSearch };

inline constexpr const char* LANDING_FAN_START = ";_DYNAPIN_LANDING_FAN_START";
inline constexpr const char* LANDING_FAN_END   = ";_DYNAPIN_LANDING_FAN_END";

struct Pin
{
    int row = 0;
    int col = 0;

    bool operator==(const Pin& rhs) const { return row == rhs.row && col == rhs.col; }
};

enum class SelectionSource { Unavailable, Manual, Automatic };

struct SelectionResult
{
    SelectionSource  source = SelectionSource::Unavailable;
    std::vector<Pin> pins;
    std::vector<Pin> rejected_collisions;
    std::string      warning;
};

enum class ProjectionEventType { Contact, PinSurface, Model };

struct ProjectionEvent
{
    ProjectionEventType type    = ProjectionEventType::Contact;
    double              print_z = 0.;
    Polygons            polygons;
    Pin                 pin;
    bool                colliding = false;
};

struct ProjectionSelection
{
    std::vector<Pin> selected;
    std::vector<Pin> rejected_collisions;
};

enum class PullType { NormalOnly, RevertedOnly, Alternating };

struct PullMoveConfig
{
    double x_hook               = 0.;
    double x_latch              = 0.;
    double x_front              = 0.;
    double fast_feed_rate       = 6000.;
    double feed_rate            = 1200.;
    double pull_feedrate_fast   = 3000.;
    double y_origin_offset      = 0.;
    double alternating_y_offset = 0.;
    double y_offset             = 0.;
    double approach_y_offset    = -4.;
    double approach_z_offset    = 5.;
    double engage_z_drop        = 5.;
    double disengage_x_offset   = 1.5;
    double disengage_z_lift     = 5.;
    double retreat_z_lift       = 0.;
};

struct PullAlternatingConfig
{
    int            reverted_row_parity = 0;
    PullMoveConfig normal;
    PullMoveConfig reverted;
};

struct PullConfig
{
    PullType              type = PullType::NormalOnly;
    PullMoveConfig        normal;
    PullAlternatingConfig alternating;
    PullMoveConfig        reverted;
};

struct SupportSpeedConfig
{
    int    layers         = 3;
    double max_x_speed    = 0.; // mm/s; 0 uses initial_layer_speed
};

struct LandingSlopeConfig
{
    double reference_x    = 25.; // machine X at the configured pin-top height
    double slope_x        = 0.;  // dz/dx; zero preserves horizontal landing
    double max_band_width = 7.5;
    double travel_clearance = 2.;
};

struct Config
{
    int            row_count        = 0;
    int            col_count        = 0;
    double         pull_origin_y    = 0.;
    double         pull_origin_z    = 0.;
    double         support_origin_y = 0.;
    double         support_origin_z = 0.;
    double         row_pitch_z      = 0.;
    double         col_pitch_y      = 0.;
    double         blocker_width_y  = 0.;
    double         blocker_height_z = 0.;
    double         landing_edge_inset_y = 0.;
    PullConfig     pull_gcode;
    SupportSpeedConfig support_speed;
    LandingSlopeConfig landing_slope;
};

struct BlockerZRange
{
    double z_min = 0.;
    double z_max = 0.;
};

// Axis-aligned 3D region (machine/world coordinates, mm) in which support
// material is excluded by a selected pin, together with the pin's own position.
// Used by the G-code preview to visualize pins and blocked regions.
struct BlockerBox
{
    Pin   pin;
    Vec3d min{Vec3d::Zero()};     // lower corner of the blocked region
    Vec3d max{Vec3d::Zero()};     // upper corner of the blocked region
    Vec3d pin_pos{Vec3d::Zero()}; // physical position of the pin
};

// A prism in which support material must be excluded. The polygon is expressed
// in the PrintObject's local (slice) coordinate system so it can be subtracted
// directly from generated support layers; [z_min, z_max] are print_z bounds (mm).
struct LocalBlocker
{
    Pin     pin;
    Polygon poly;
    double  z_min = 0.;
    double  z_max = 0.;
};

// DynaPin ピンの上面を表す仮想サポート面。
// サポート材がビルドプレートまで降りる代わりに、この面で「着地」できる。
// PrintObject のローカル（スライス）座標系で表現される。
// print_z はピン上面の高さ (mm)。
struct VirtualSupportSurface
{
    Pin     pin;          // ピン識別子。着地後の枝とG-code出力を対応付ける。
    Polygon poly;         // ピン上面の XY 形状（object-local 座標）
    double  print_z = 0.; // ピン上面の Z 高さ (mm)
    double  edge_inset_y = 0.; // 着地面の外縁余白。隣接ピンとの共有境界には適用しない。
    double  source_print_z = 0.; // Original horizontal landing envelope, before band alignment.
};

struct LandingLayer
{
    double   bottom_z;
    double   print_z;
    Polygons polygons;
};

struct LandingLayerAdjustment
{
    size_t                    source;
    std::vector<LandingLayer> layers;
};

// Replace only the first landing footprint, keeping its upper Z fixed. The
// replacement starts on the pin and respects the support layer height limits.
std::vector<LandingLayerAdjustment> align_landing_layers(const std::vector<LandingLayer>&          layers,
                                                         const std::vector<VirtualSupportSurface>& surfaces,
                                                         double                                    min_height,
                                                         double                                    max_height,
                                                         double                                    expansion = 0.);

// Result indices match layers; coincident Z slabs count as a single layer.
std::vector<Polygons> landing_regions(const std::vector<LandingLayer>&          layers,
                                      const std::vector<VirtualSupportSurface>& surfaces,
                                      size_t                                    layer_count);
// Count support layers above each actual landing footprint, excluding the base itself.
std::vector<Polygons> stabilization_regions(const std::vector<LandingLayer>& layers, const std::vector<Polygons>& bases, size_t layer_count);
// First-layer footprints expanded only within the corresponding pin surface.
std::vector<LandingLayer> landing_base_layers(const std::vector<LandingLayer>&          layers,
                                              const std::vector<VirtualSupportSurface>& surfaces,
                                              double                                    expansion);
Polygons                  landing_printable_region(const PrintObject& object);
void                  mark_landing_paths(ExtrusionEntityCollection& collection, const Polygons& regions);

struct TreePinLanding
{
    Pin      pin;
    uint64_t branch_id = 0;
    size_t   layer_id  = 0;
    Point    position;
    Polygon  footprint;
    double   print_z = 0.;
    double   support_print_z = 0.;
};

struct TreePinLandingPlan
{
    std::vector<TreePinLanding> landings;
    std::vector<Pin>            used_pins;
};

std::vector<Pin>        parse_pin_list(const std::string& pins);
bool                    has_manual_selection(const Print& print);
std::vector<Pin>        candidate_pins(const Config& config);
void                    sort_unique_pins(std::vector<Pin>& pins, const Config& config);
ProjectionSelection     select_from_projection(std::vector<ProjectionEvent> events);
ProjectionSelection     select_tree_pins(const PrintObject&      object,
                                         const Config&           config,
                                         const std::vector<Pin>& candidates,
                                         const std::vector<Pin>& colliding);
const std::vector<Pin>& resolved_pins(const Print& print);
bool                    load_config_for_print(const Print& print, Config& config, std::string* error = nullptr);
LocalBlocker            blocker_for_pin(const PrintObject& object, const Config& config, const Pin& pin);
VirtualSupportSurface   surface_for_pin(const PrintObject& object, const Config& config, const Pin& pin);
// Routing keeps one conservative horizontal envelope per pin; only landing
// generation consumes the disjoint horizontal bands below that envelope.
std::vector<VirtualSupportSurface> landing_surfaces_for_pin(const PrintObject& object, const Config& config, const Pin& pin);
std::vector<VirtualSupportSurface> landing_surfaces_for_object(const PrintObject& object);
double                  pin_top_z_at_x(const Config& config, const Pin& pin, double machine_x);
double                  pin_pull_trigger_z(const Print& print, const Config& config, const Pin& pin);
double                  pin_travel_clearance_z(const Print& print, const Config& config, const std::vector<Pin>& pins);
std::optional<Point>    nearest_pin_landing_point(const Polygon& safe_region, const Point& from);
bool                    pin_collides_with_model(const Print& print, const Config& config, const Pin& pin,
                                               std::size_t* intersection_count = nullptr);
std::vector<Polygons>   support_blockers_for_object(const PrintObject& object);
// Blocker prisms in object-local coordinates, used to clip already-generated
// support layers (e.g. support columns descending through the blocked region
// from overhangs located above it).
std::vector<LocalBlocker> support_blocker_regions_local(const PrintObject& object);
// 選択された各ピンの上面を仮想サポート面として返す（object-local 座標）。
// これらの面は「仮想ビルドプレート」として機能し、ピン上面より上で発生した
// サポート材がビルドプレートまで降りずにピン上面で止まるようにする。
std::vector<VirtualSupportSurface> pin_top_surfaces_for_object(const PrintObject& object);
// Returns one box per selected pin (empty when DynaPin optimization is disabled
// or the config cannot be loaded).
std::vector<BlockerBox> selected_blocker_boxes(const Print& print);
std::string             pull_gcode_for_pin(const Config& config, const Pin& pin);
double                  pin_y(const Config& config, const Pin& pin);
double                  support_x_front(const Config& config);
BlockerZRange           blocker_z_range(const Config& config, const Pin& pin);

// The support generator and G-code writer must use the same stage override.
// 0 = normal support, 1 = landing surfaces, 2 = full DynaPin support/pulls.
int                     effective_debug_stage(const Print& print);

} // namespace DynaPin
} // namespace Slic3r

#endif
