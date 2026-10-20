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

TEST_CASE("Capture resizing preserves vertical framing", "[preview_capture]")
{
    PreviewCapture original;
    original.width    = 1600;
    original.height   = 900;
    original.position = {150, -180, 180};
    original.target   = {50, 50, 0};
    original.zoom     = 2.0;
    const BoundingBoxf3 bounds(Vec3d::Zero(), Vec3d(100, 100, 100));
    for (bool perspective : {false, true}) {
        original.perspective = perspective;
        Camera before;
        before.restore_view(original, Vec3d::Zero());
        before.apply_projection(bounds, -1, -1, true);
        for (const auto& size : {std::pair<unsigned, unsigned>{800, 450}, {900, 900}, {450, 800}, {1600, 1200}}) {
            const auto resized = original.resized(size.first, size.second);
            Camera     after;
            after.restore_view(resized, Vec3d::Zero());
            after.apply_projection(bounds, -1, -1, true);
            REQUIRE(after.get_position().isApprox(before.get_position()));
            REQUIRE(after.get_target().isApprox(before.get_target()));
            REQUIRE(resized.zoom == Catch::Approx(original.zoom * size.second / original.height));
            const Vec4d point(50, 40, 10, 1);
            const Vec4d a = before.get_projection_matrix().matrix() * before.get_view_matrix().matrix() * point;
            const Vec4d b = after.get_projection_matrix().matrix() * after.get_view_matrix().matrix() * point;
            REQUIRE(b.y() / b.w() == Catch::Approx(a.y() / a.w()));
            const double aspect_before = double(original.width) / original.height;
            const double aspect_after  = double(resized.width) / resized.height;
            REQUIRE(b.x() / b.w() * aspect_after == Catch::Approx(a.x() / a.w() * aspect_before));
        }
    }
    REQUIRE_THROWS(original.resized(0, 900));
    REQUIRE_THROWS(original.resized(16385, 900));
    REQUIRE_THROWS(original.resized(10000, 10000));
    REQUIRE(original.width == 1600);
    REQUIRE(original.height == 900);
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

// Unsliced capture must work before any slicing and include only instances on the target plate.
TEST_CASE("Render unsliced models without toolpaths", "[.preview_capture_render]")
{
    const auto old_resources = resources_dir();
    const auto old_var_dir   = var_dir();
    set_resources_dir((boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() / "resources").string());
    set_var_dir((boost::filesystem::path(resources_dir()) / "images").string());
    ScopeGuard restore_resources([&] {
        set_resources_dir(old_resources);
        set_var_dir(old_var_dir);
    });
    Model      model;
    auto*      object = model.add_object();
    object->add_volume(TriangleMesh(its_make_cube(20, 20, 30)));
    object->add_instance()->set_offset(Vec3d(40, 40, 0));
    Print                print;
    GCodeProcessorResult result;
    DynamicPrintConfig   config;
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#FF8000"}));
    print.apply_config_for_render(config);
    PartPlate plate(nullptr, Vec3d::Zero(), 100, 100, 100, nullptr, &model, true, ptFFF);
    plate.set_shape({{0, 0}, {100, 0}, {100, 100}, {0, 100}}, {}, {}, {}, Vec2d::Zero(), 0, 0);
    plate.add_instance(0, 0, false);
    PreviewCapture capture;
    capture.width    = 640;
    capture.height   = 360;
    capture.position = {150, -180, 180};
    capture.target   = {50, 50, 0};
    capture.zoom     = 2;
    capture.bed      = false;
    const auto path  = boost::filesystem::temp_directory_path() / "orca-unsliced-capture.png";
    export_preview_image_cli(path.string(), capture, print, result, plate, true);
    wxImage original;
    REQUIRE(original.LoadFile(wxString::FromUTF8(path.string()), wxBITMAP_TYPE_PNG));
    REQUIRE(original.GetWidth() == 640);
    REQUIRE(original.GetHeight() == 360);
    size_t foreground = 0;
    for (size_t i = 0; i < 640 * 360 * 3; i += 3)
        if (original.GetData()[i] > original.GetData()[i + 1] + 20)
            ++foreground;
    REQUIRE(foreground > 100);

    // An overlapping instance belonging to another plate and non-printing volumes must not change the image.
    object->add_instance()->set_offset(Vec3d(40, 40, 40));
    auto* modifier = object->add_volume(TriangleMesh(its_make_cube(80, 80, 80)));
    modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    const auto filtered_path = boost::filesystem::temp_directory_path() / "orca-unsliced-filtered.png";
    export_preview_image_cli(filtered_path.string(), capture, print, result, plate, true);
    wxImage filtered;
    REQUIRE(filtered.LoadFile(wxString::FromUTF8(filtered_path.string()), wxBITMAP_TYPE_PNG));
    REQUIRE(std::equal(original.GetData(), original.GetData() + 640 * 360 * 3, filtered.GetData()));

    const Vec3d offset(250, -250, 0);
    object->instances[0]->set_offset(Vec3d(40, 40, 0) + offset);
    PartPlate shifted(nullptr, offset, 100, 100, 100, nullptr, &model, true, ptFFF);
    shifted.set_shape({{0, 0}, {100, 0}, {100, 100}, {0, 100}}, {}, {}, {}, offset.head<2>(), 0, 0);
    shifted.add_instance(0, 0, false);
    export_preview_image_cli(filtered_path.string(), capture, print, result, shifted, true);
    REQUIRE(filtered.LoadFile(wxString::FromUTF8(filtered_path.string()), wxBITMAP_TYPE_PNG));
    size_t changed = 0;
    for (size_t i = 0; i < 640 * 360 * 3; ++i)
        if (std::abs(int(original.GetData()[i]) - int(filtered.GetData()[i])) > 2)
            ++changed;
    REQUIRE(changed < 640 * 360 * 3 / 100);
}
