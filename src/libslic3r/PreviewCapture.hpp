#ifndef slic3r_PreviewCapture_hpp_
#define slic3r_PreviewCapture_hpp_

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <cereal/types/array.hpp>
#include <cereal/types/optional.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>
#include <nlohmann/json_fwd.hpp>

namespace Slic3r {

struct PreviewCaptureTexture
{
    std::array<float, 4>         rectangle{};
    std::string                  file;
    template<class Archive> void serialize(Archive& ar) { ar(rectangle, file); }
};
void to_json(nlohmann::json& json, const PreviewCaptureTexture& texture);
void from_json(const nlohmann::json& json, PreviewCaptureTexture& texture);

// Project display metadata; never part of DynamicPrintConfig or slicing inputs.
struct PreviewCapture
{
    std::array<double, 3>                     position{0, -1000, 1000};
    std::array<double, 3>                     target{0, 0, 0};
    std::array<double, 3>                     up{0, 0, 1};
    double                                    zoom        = 1.0;
    bool                                      perspective = true;
    unsigned int                              width = 1, height = 1;
    std::array<float, 4>                      background{0.906f, 0.906f, 0.906f, 1.0f};
    std::vector<std::array<unsigned char, 3>> role_colors;
    bool                                      dark = false, bed = true, axes = false, grid = true, dynapin = true;
    std::string                               bed_model, bed_texture;
    std::vector<PreviewCaptureTexture>        bed_parts;
    bool                                      shells        = true;
    float                                     shell_opacity = 0.15f;

    void                         validate() const;
    PreviewCapture               resized(unsigned int width, unsigned int height) const;
    template<class Archive> void serialize(Archive& ar)
    {
        ar(position, target, up, zoom, perspective, width, height, background, role_colors, dark, bed, axes, grid, dynapin, bed_model,
           bed_texture, shells, shell_opacity, bed_parts);
    }
};

void                          to_json(nlohmann::json& json, const PreviewCapture& capture);
void                          from_json(const nlohmann::json& json, PreviewCapture& capture);
std::string                   serialize_preview_captures(const std::map<int, PreviewCapture>& captures);
std::map<int, PreviewCapture> deserialize_preview_captures(const std::string& text);
inline constexpr const char*  PREVIEW_CAPTURE_FILE = "Metadata/orca_preview_capture.json";

} // namespace Slic3r
#endif
