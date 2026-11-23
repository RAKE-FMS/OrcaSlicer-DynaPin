#ifndef slic3r_DynaPinPrintTypes_hpp_
#define slic3r_DynaPinPrintTypes_hpp_

#include "Polygon.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::DynaPin {

enum class SupportGeometryMode { Exact, PlacementSearch };

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

} // namespace Slic3r::DynaPin

#endif
