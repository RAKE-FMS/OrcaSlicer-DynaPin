#include <catch2/catch_all.hpp>
#include "libslic3r/PreviewCapture.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <cereal/archives/binary.hpp>
#include <sstream>
#include <miniz.h>

using namespace Slic3r;

TEST_CASE("Preview capture metadata preserves plate identity and framing", "[preview_capture]")
{
    PreviewCapture a;
    a.width       = 1440;
    a.height      = 900;
    a.position    = {10, -340, 150};
    a.target      = {90, 90, 30};
    a.zoom        = 3.25;
    a.role_colors = {{255, 128, 0}, {0, 255, 0}};
    a.bed_model   = "models/bed.stl";
    a.bed_parts.push_back({{10, 20, 30, 40}, "images/bed.svg"});
    auto b              = a;
    b.perspective       = false;
    b.axes              = true;
    b.dynapin           = false;
    const auto text     = serialize_preview_captures({{0, a}, {3, b}});
    const auto restored = deserialize_preview_captures(text);
    REQUIRE(restored.size() == 2);
    REQUIRE(nlohmann::json(restored.at(0)) == nlohmann::json(a));
    REQUIRE(nlohmann::json(restored.at(3)) == nlohmann::json(b));
    REQUIRE_FALSE(restored.count(1));
}

TEST_CASE("Invalid capture metadata cannot reach camera math or GPU allocation", "[preview_capture]")
{
    PreviewCapture c;
    SECTION("zero size") { c.width = 0; }
    SECTION("excessive pixels")
    {
        c.width  = 16384;
        c.height = 16384;
    }
    SECTION("coincident camera and target") { c.position = c.target; }
    SECTION("parallel up direction") { c.up = {0, -1, 1}; }
    SECTION("non-finite zoom") { c.zoom = std::numeric_limits<double>::infinity(); }
    SECTION("non-finite position") { c.position[0] = std::numeric_limits<double>::quiet_NaN(); }
    REQUIRE_THROWS(c.validate());
}

TEST_CASE("Unsupported preview metadata versions are rejected", "[preview_capture]")
{
    REQUIRE_THROWS(deserialize_preview_captures(R"({"version":2,"plates":[]})"));
    REQUIRE_THROWS(deserialize_preview_captures("{"));
    REQUIRE_THROWS(deserialize_preview_captures(R"({"version":1,"plates":[{"plate":0}]})"));
}

TEST_CASE("Capture metadata participates in undo serialization independently of print config", "[preview_capture]")
{
    PreviewCapture capture;
    capture.width                          = 800;
    capture.height                         = 600;
    capture.target                         = {25, 35, 45};
    std::optional<PreviewCapture> recorded = capture;
    std::stringstream             stream;
    {
        cereal::BinaryOutputArchive archive(stream);
        archive(recorded);
    }
    recorded.reset();
    {
        cereal::BinaryInputArchive archive(stream);
        archive(recorded);
    }
    REQUIRE(recorded.has_value());
    REQUIRE(nlohmann::json(*recorded) == nlohmann::json(capture));
}

static void replace_capture_metadata(const std::string& path, const std::optional<std::string>& capture_text)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    REQUIRE(mz_zip_reader_init_file(&archive, path.c_str(), 0));
    std::vector<std::pair<std::string, std::string>> entries;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&archive, i, &stat));
        if (std::string(stat.m_filename) == PREVIEW_CAPTURE_FILE)
            continue;
        std::string contents(size_t(stat.m_uncomp_size), '\0');
        REQUIRE(mz_zip_reader_extract_to_mem(&archive, i, contents.data(), contents.size(), 0));
        entries.emplace_back(stat.m_filename, std::move(contents));
    }
    REQUIRE(mz_zip_reader_end(&archive));
    mz_zip_zero_struct(&archive);
    REQUIRE(mz_zip_writer_init_file(&archive, path.c_str(), 0));
    if (capture_text)
        REQUIRE(mz_zip_writer_add_mem(&archive, PREVIEW_CAPTURE_FILE, capture_text->data(), capture_text->size(), MZ_DEFAULT_COMPRESSION));
    for (const auto& [name, contents] : entries)
        REQUIRE(mz_zip_writer_add_mem(&archive, name.c_str(), contents.data(), contents.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&archive));
    REQUIRE(mz_zip_writer_end(&archive));
}

TEST_CASE("Capture settings survive a project 3MF round trip", "[preview_capture][3mf]")
{
    const auto old_temporary_dir = temporary_dir();
    set_temporary_dir(boost::filesystem::temp_directory_path().string());
    ScopeGuard restore_temporary_dir([&] { set_temporary_dir(old_temporary_dir); });
    const auto filename = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("capture-%%%%-%%%%.3mf");
    ScopeGuard cleanup([&] { boost::filesystem::remove(filename); });
    Model      model;
    REQUIRE(load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &model));
    model.add_default_instances();
    DynamicPrintConfig config;
    PlateData          first, second;
    first.plate_index = 0;
    first.objects_and_instances.emplace_back(0, 0);
    second.plate_index = 1;
    second.preview_capture.emplace();
    second.preview_capture->width  = 800;
    second.preview_capture->height = 600;
    second.preview_capture->zoom   = 2.5;
    StoreParams       params;
    const std::string path = filename.string();
    params.path            = path.c_str();
    params.model           = &model;
    params.config          = &config;
    params.plate_data_list = {&first, &second};
    params.strategy        = SaveStrategy::Zip64;
    bool expect_capture    = true;
    SECTION("Plate metadata stored out of order") { params.plate_data_list = {&second, &first}; }
    SECTION("Single plate export keeps only the selected capture")
    {
        first.preview_capture.emplace();
        params.export_plate_idx = 1;
    }
    SECTION("Exporting a different plate omits the capture")
    {
        params.export_plate_idx = 0;
        expect_capture          = false;
    }
    REQUIRE(store_bbs_3mf(params));
    SECTION("Recorded metadata round trip") {}
    SECTION("Old project without capture metadata")
    {
        replace_capture_metadata(path, std::nullopt);
        expect_capture = false;
    }
    SECTION("Malformed capture metadata leaves model loadable")
    {
        replace_capture_metadata(path, "{broken");
        expect_capture = false;
    }
    SECTION("Unsupported capture version leaves model loadable")
    {
        replace_capture_metadata(path, R"({"version":2,"plates":[]})");
        expect_capture = false;
    }

    Model                     restored;
    PlateDataPtrs             plates;
    std::vector<Preset*>      presets;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
    bool                      bbl = false;
    Semver                    version;
    REQUIRE(load_bbs_3mf(path.c_str(), &config, &substitutions, &restored, &plates, &presets, &bbl, &version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(plates.size() == 2);
    REQUIRE_FALSE(plates[0]->preview_capture);
    REQUIRE(plates[1]->preview_capture.has_value() == expect_capture);
    if (expect_capture)
        REQUIRE(nlohmann::json(*plates[1]->preview_capture) == nlohmann::json(*second.preview_capture));
    REQUIRE(restored.objects.size() == model.objects.size());
    release_PlateData_list(plates);
    for (auto* preset : presets)
        delete preset;
}
