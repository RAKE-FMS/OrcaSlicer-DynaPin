#include "PreviewCapture.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <stdexcept>

namespace Slic3r {

void PreviewCapture::validate() const
{
    if (width == 0 || height == 0 || width > 16384 || height > 16384 || uint64_t(width) * height > 64000000 || !std::isfinite(zoom) ||
        zoom <= 0.0)
        throw std::runtime_error("Invalid preview capture size or zoom");
    for (const auto& texture : bed_parts)
        for (float value : texture.rectangle)
            if (!std::isfinite(value))
                throw std::runtime_error("Invalid preview bed texture rectangle");
    double distance_sq = 0.0, cross_sq = 0.0;
    for (size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(position[i]) || !std::isfinite(target[i]) || !std::isfinite(up[i]))
            throw std::runtime_error("Invalid preview capture camera");
        const double delta = position[i] - target[i];
        distance_sq += delta * delta;
        const size_t j = (i + 1) % 3, k = (i + 2) % 3;
        const double cross = up[j] * (position[k] - target[k]) - up[k] * (position[j] - target[j]);
        cross_sq += cross * cross;
    }
    if (!std::isfinite(distance_sq) || !std::isfinite(cross_sq) || distance_sq < 1e-12 || cross_sq < 1e-12)
        throw std::runtime_error("Degenerate preview capture camera");
    if (!std::isfinite(shell_opacity) || shell_opacity < 0 || shell_opacity > 1)
        throw std::runtime_error("Invalid preview shell opacity");
    for (float c : background)
        if (!std::isfinite(c) || c < 0.0f || c > 1.0f)
            throw std::runtime_error("Invalid preview capture background");
}

PreviewCapture PreviewCapture::resized(unsigned int new_width, unsigned int new_height) const
{
    auto capture = *this;
    capture.zoom *= double(new_height) / height;
    capture.width  = new_width;
    capture.height = new_height;
    capture.validate();
    return capture;
}

void to_json(nlohmann::json& j, const PreviewCaptureTexture& texture) { j = {{"rectangle", texture.rectangle}, {"file", texture.file}}; }
void from_json(const nlohmann::json& j, PreviewCaptureTexture& texture)
{
    j.at("rectangle").get_to(texture.rectangle);
    j.at("file").get_to(texture.file);
}

void to_json(nlohmann::json& j, const PreviewCapture& c)
{
    j = {{"position", c.position},
         {"target", c.target},
         {"up", c.up},
         {"zoom", c.zoom},
         {"perspective", c.perspective},
         {"width", c.width},
         {"height", c.height},
         {"background", c.background},
         {"role_colors", c.role_colors},
         {"dark", c.dark},
         {"bed", c.bed},
         {"axes", c.axes},
         {"grid", c.grid},
         {"dynapin", c.dynapin},
         {"bed_model", c.bed_model},
         {"bed_texture", c.bed_texture},
         {"shells", c.shells},
         {"shell_opacity", c.shell_opacity},
         {"bed_parts", c.bed_parts}};
}

void from_json(const nlohmann::json& j, PreviewCapture& c)
{
    j.at("position").get_to(c.position);
    j.at("target").get_to(c.target);
    j.at("up").get_to(c.up);
    j.at("zoom").get_to(c.zoom);
    j.at("perspective").get_to(c.perspective);
    j.at("width").get_to(c.width);
    j.at("height").get_to(c.height);
    j.at("background").get_to(c.background);
    j.at("role_colors").get_to(c.role_colors);
    j.at("dark").get_to(c.dark);
    j.at("bed").get_to(c.bed);
    j.at("axes").get_to(c.axes);
    j.at("grid").get_to(c.grid);
    j.at("dynapin").get_to(c.dynapin);
    c.bed_model     = j.value("bed_model", std::string());
    c.bed_texture   = j.value("bed_texture", std::string());
    c.shells        = j.value("shells", true);
    c.shell_opacity = j.value("shell_opacity", 0.15f);
    c.bed_parts     = j.value("bed_parts", std::vector<PreviewCaptureTexture>());
    c.validate();
}

std::string serialize_preview_captures(const std::map<int, PreviewCapture>& captures)
{
    nlohmann::json plates = nlohmann::json::array();
    for (const auto& [index, capture] : captures) {
        capture.validate();
        plates.push_back({{"plate", index}, {"capture", capture}});
    }
    return nlohmann::json({{"version", 1}, {"plates", plates}}).dump();
}

std::map<int, PreviewCapture> deserialize_preview_captures(const std::string& text)
{
    const auto j = nlohmann::json::parse(text);
    if (j.at("version") != 1)
        throw std::runtime_error("Unsupported preview capture version");
    std::map<int, PreviewCapture> result;
    for (const auto& plate : j.at("plates")) {
        const int index = plate.at("plate").get<int>();
        if (index < 0 || !result.emplace(index, plate.at("capture").get<PreviewCapture>()).second)
            throw std::runtime_error("Invalid or duplicate preview capture plate");
    }
    return result;
}

} // namespace Slic3r
