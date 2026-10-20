#ifndef slic3r_PreviewCaptureRenderer_hpp_
#define slic3r_PreviewCaptureRenderer_hpp_

#include "libslic3r/PreviewCapture.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include <string>

namespace Slic3r {
class Print;
struct GCodeProcessorResult;
namespace GUI {
class PartPlate;
class OpenGLManager;

// Requires a current GL context; pixels use OpenGL's bottom-up RGBA row order.
ThumbnailData render_preview_image(
    const PreviewCapture& capture, const Print& print, const GCodeProcessorResult& result, PartPlate& plate, OpenGLManager& gl);
void save_preview_image(const std::string& path, const ThumbnailData& image);
// Requires a current GL context. All capture GPU resources are released before returning.
void export_preview_image(const std::string&          path,
                          const PreviewCapture&       capture,
                          const Print&                print,
                          const GCodeProcessorResult& result,
                          PartPlate&                  plate,
                          OpenGLManager&              gl);
// Owns a hidden context; errors are reported to the CLI caller as exceptions.
void export_preview_image_cli(
    const std::string& path, const PreviewCapture& capture, const Print& print, const GCodeProcessorResult& result, PartPlate& plate);
std::string preview_capture_asset_path(const std::string& path);
} // namespace GUI
} // namespace Slic3r
#endif
