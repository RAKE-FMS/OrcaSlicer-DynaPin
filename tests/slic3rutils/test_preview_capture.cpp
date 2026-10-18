#include <catch2/catch_all.hpp>
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/PreviewCaptureRenderer.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <wx/image.h>

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("Capture camera preserves plate-relative framing", "[preview_capture]")
{
    Camera camera;
    camera.set_scene_box(BoundingBoxf3(Vec3d::Zero(), Vec3d(180, 180, 180)));
    camera.look_at(Vec3d(100, -300, 200), Vec3d(90, 90, 40), Vec3d::UnitZ());
    camera.set_viewport(0, 0, 1600, 900);
    camera.set_zoom(2.75);
    const Vec3d first_origin(250, -250, 0);
    auto        capture = camera.capture_view(first_origin);
    Camera      restored;
    const Vec3d second_origin(500, -500, 0);
    restored.restore_view(capture, second_origin);
    REQUIRE(restored.get_position().isApprox(camera.get_position() + second_origin - first_origin));
    REQUIRE(restored.get_target().isApprox(camera.get_target() + second_origin - first_origin));
    REQUIRE(restored.get_dir_up().isApprox(camera.get_dir_up()));
    REQUIRE(restored.get_zoom() == Catch::Approx(2.75));
    REQUIRE(restored.get_viewport()[2] == 1600);
    REQUIRE(restored.get_viewport()[3] == 900);
    REQUIRE(restored.get_type() == Camera::EType::Perspective);
    capture.perspective = false;
    restored.restore_view(capture, second_origin);
    REQUIRE(restored.get_type() == Camera::EType::Ortho);
    const auto position = restored.get_position();
    const auto target   = restored.get_target();
    // Large scene bounds must change clipping planes without moving the recorded camera.
    restored.apply_projection(BoundingBoxf3(Vec3d(-10000, -10000, -10000), Vec3d(10000, 10000, 10000)), -1, -1, true);
    REQUIRE(restored.get_position().isApprox(position));
    REQUIRE(restored.get_target().isApprox(target));
}

// Explicit opt-in: this integration test requires a working desktop OpenGL context.
TEST_CASE("Render preview PNG through hidden CLI context", "[.preview_capture_render]")
{
    const auto old_resources = resources_dir();
    const auto old_var_dir   = var_dir();
    set_resources_dir((boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() / "resources").string());
    set_var_dir((boost::filesystem::path(resources_dir()) / "images").string());
    ScopeGuard           restore_resources([&] {
        set_resources_dir(old_resources);
        set_var_dir(old_var_dir);
    });
    const auto           path = boost::filesystem::temp_directory_path() / "orca-preview-capture-smoke.png";
    Print                print;
    GCodeProcessorResult result;
    result.extruder_colors    = {"#FF8000"};
    result.filament_densities = {1.24f};
    result.moves.emplace_back();
    for (const Vec3f& p : {Vec3f(20, 20, 0.2f), Vec3f(80, 20, 0.2f), Vec3f(80, 80, 0.2f), Vec3f(20, 80, 0.2f), Vec3f(20, 20, 0.2f)}) {
        GCodeProcessorResult::MoveVertex move;
        move.type           = EMoveType::Extrude;
        move.extrusion_role = erPerimeter;
        move.position       = p;
        move.width          = 0.45f;
        move.height         = 0.2f;
        move.mm3_per_mm     = 0.09f;
        move.feedrate       = 20;
        move.time           = {1, 0};
        move.gcode_id       = unsigned(result.moves.size());
        result.moves.push_back(move);
    }
    PartPlate plate(nullptr, Vec3d::Zero(), 100, 100, 100, nullptr, nullptr, true, ptFFF);
    plate.set_shape({{0, 0}, {100, 0}, {100, 100}, {0, 100}}, {}, {}, {}, Vec2d::Zero(), 0, 0);
    PreviewCapture capture;
    capture.width       = 640;
    capture.height      = 360;
    capture.position    = {150, -180, 180};
    capture.target      = {50, 50, 0};
    capture.zoom        = 2.0;
    capture.axes        = true;
    capture.bed_texture = "profiles/Artillery/artillery_geniuspro_buildplate_texture.png";
    capture.bed_parts.push_back({{5, 5, 30, 30}, "images/orca_bed_pct_left.svg"});
    export_preview_image_cli(path.string(), capture, print, result, plate);
    boost::nowide::ifstream       stream(path.string(), std::ios::binary);
    std::array<unsigned char, 24> header{};
    stream.read(reinterpret_cast<char*>(header.data()), header.size());
    REQUIRE(stream.good());
    REQUIRE(header[0] == 137);
    REQUIRE(header[1] == 'P');
    REQUIRE(header[2] == 'N');
    REQUIRE(header[3] == 'G');
    auto dimension = [&](size_t offset) {
        return uint32_t(header[offset]) << 24 | uint32_t(header[offset + 1]) << 16 | uint32_t(header[offset + 2]) << 8 |
               uint32_t(header[offset + 3]);
    };
    REQUIRE(dimension(16) == capture.width);
    REQUIRE(dimension(20) == capture.height);
    REQUIRE(boost::filesystem::file_size(path) > 1024);

    // The same recorded composition must survive a different GUI/CLI plate layout.
    const Vec3d offset(250, -250, 0);
    for (auto& move : result.moves)
        move.position += offset.cast<float>();
    PartPlate shifted_plate(nullptr, offset, 100, 100, 100, nullptr, nullptr, true, ptFFF);
    shifted_plate.set_shape({{0, 0}, {100, 0}, {100, 100}, {0, 100}}, {}, {}, {}, offset.head<2>(), 0, 0);
    const auto shifted_path = boost::filesystem::temp_directory_path() / "orca-preview-capture-shifted.png";
    export_preview_image_cli(shifted_path.string(), capture, print, result, shifted_plate);
    wxImage original, shifted;
    REQUIRE(original.LoadFile(wxString::FromUTF8(path.string()), wxBITMAP_TYPE_PNG));
    REQUIRE(shifted.LoadFile(wxString::FromUTF8(shifted_path.string()), wxBITMAP_TYPE_PNG));
    REQUIRE(shifted.GetSize() == original.GetSize());
    const size_t byte_count = size_t(capture.width) * capture.height * 3;
    size_t       changed    = 0;
    for (size_t i = 0; i < byte_count; ++i)
        if (std::abs(int(original.GetData()[i]) - int(shifted.GetData()[i])) > 2)
            ++changed;
    REQUIRE(changed < byte_count / 100);
}
