#include "PreviewCaptureRenderer.hpp"
#include "Camera.hpp"
#include "GCodeViewer.hpp"
#include "OpenGLManager.hpp"
#include "PartPlate.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <wx/imagpng.h>
#include <stdexcept>

namespace Slic3r::GUI {
namespace {
// Export may also run in the GUI's current context. Restore the caller's GL state on success and failure.
struct CaptureGLState
{
    GLint     draw_fbo, read_fbo, program, viewport[4], vao, array_buffer, renderbuffer;
    GLint     active_texture, texture0, active_binding, pack_alignment, pack_buffer, pack_row_length, pack_skip_rows, pack_skip_pixels;
    GLint     blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha, depth_func, front_face, cull_mode;
    GLfloat   clear_color[4], line_width;
    GLint     unpack_alignment, unpack_row_length, unpack_skip_rows, unpack_skip_pixels, unpack_buffer, texture_buffer;
    GLboolean depth, blend, cull, multisample, scissor, depth_mask, color_mask[4];
    GLuint    fbo = 0, color = 0, depth_buffer = 0;
    CaptureGLState()
    {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &active_binding);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &pack_skip_rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &pack_skip_pixels);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
        glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
        glGetIntegerv(GL_FRONT_FACE, &front_face);
        glGetIntegerv(GL_CULL_FACE_MODE, &cull_mode);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
        glGetFloatv(GL_LINE_WIDTH, &line_width);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
        glGetIntegerv(GL_TEXTURE_BUFFER_BINDING, &texture_buffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
        depth       = glIsEnabled(GL_DEPTH_TEST);
        blend       = glIsEnabled(GL_BLEND);
        cull        = glIsEnabled(GL_CULL_FACE);
        multisample = glIsEnabled(GL_MULTISAMPLE);
        scissor     = glIsEnabled(GL_SCISSOR_TEST);
    }
    ~CaptureGLState()
    {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        glDeleteFramebuffers(1, &fbo);
        glDeleteRenderbuffers(1, &color);
        glDeleteRenderbuffers(1, &depth_buffer);
        glUseProgram(program);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture0);
        glActiveTexture(active_texture);
        glBindTexture(GL_TEXTURE_2D, active_binding);
        glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_length);
        glPixelStorei(GL_PACK_SKIP_ROWS, pack_skip_rows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, pack_skip_pixels);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
        glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha);
        glDepthFunc(depth_func);
        glFrontFace(front_face);
        glCullFace(cull_mode);
        glDepthMask(depth_mask);
        glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
        glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
        glLineWidth(line_width);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
        glBindBuffer(GL_TEXTURE_BUFFER, texture_buffer);
        auto restore = [](GLenum cap, GLboolean enabled) {
            if (enabled)
                glEnable(cap);
            else
                glDisable(cap);
        };
        restore(GL_DEPTH_TEST, depth);
        restore(GL_BLEND, blend);
        restore(GL_CULL_FACE, cull);
        restore(GL_MULTISAMPLE, multisample);
        restore(GL_SCISSOR_TEST, scissor);
    }
};

struct HiddenCaptureContext
{
    GLFWwindow* window   = nullptr;
    GLFWwindow* previous = nullptr;
    HiddenCaptureContext()
    {
        if (!glfwInit())
            throw std::runtime_error("Cannot initialize GLFW for preview capture");
        previous = glfwGetCurrentContext();
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
        window = glfwCreateWindow(64, 64, "Preview capture", nullptr, nullptr);
        if (!window) {
            glfwTerminate();
            throw std::runtime_error("Cannot create OpenGL 3.2 preview capture context");
        }
        glfwMakeContextCurrent(window);
    }
    ~HiddenCaptureContext()
    {
        glfwMakeContextCurrent(previous);
        glfwDestroyWindow(window);
        // This entry point is used before the CLI thumbnail context is created.
        if (!previous)
            glfwTerminate();
    }
};

void load_capture_texture(GLTexture& texture, const std::string& file)
{
    const bool loaded = boost::algorithm::iends_with(file, ".svg") ? texture.load_from_svg_file(file, false, false, false, 2048) :
                                                                     texture.load_from_file(file, false, GLTexture::None, false);
    if (!loaded)
        throw std::runtime_error("Cannot load preview bed texture: " + file);
}

std::string resolve_asset(const std::string& path)
{
    if (path.empty() || boost::filesystem::path(path).is_absolute())
        return path;
    return (boost::filesystem::path(resources_dir()) / path).string();
}
} // namespace

std::string preview_capture_asset_path(const std::string& path)
{
    const auto relative = boost::filesystem::path(path).lexically_relative(resources_dir());
    if (!path.empty() && !relative.empty() && *relative.begin() != "..")
        return relative.generic_string();
    return path;
}

void export_preview_image(const std::string&          path,
                          const PreviewCapture&       capture,
                          const Print&                print,
                          const GCodeProcessorResult& result,
                          PartPlate&                  source_plate,
                          OpenGLManager&              gl)
{
    capture.validate();
    if (result.moves.empty())
        throw std::runtime_error("No toolpaths to capture");
    for (const char* shader : {"flat", "hotbed", "printbed", "gouraud_light"})
        if (!gl.get_shader(shader))
            throw std::runtime_error(std::string("Preview capture shader is unavailable: ") + shader);
    GLint max_size = 0;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_size);
    if (capture.width > unsigned(max_size) || capture.height > unsigned(max_size))
        throw std::runtime_error("Preview image exceeds the GPU renderbuffer size limit");
    CaptureGLState state;
    glGenFramebuffers(1, &state.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, state.fbo);
    glGenRenderbuffers(1, &state.color);
    glBindRenderbuffer(GL_RENDERBUFFER, state.color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, capture.width, capture.height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, state.color);
    glGenRenderbuffers(1, &state.depth_buffer);
    glBindRenderbuffer(GL_RENDERBUFFER, state.depth_buffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, capture.width, capture.height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, state.depth_buffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Preview capture framebuffer is incomplete");
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_MULTISAMPLE);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(capture.background[0], capture.background[1], capture.background[2], capture.background[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    GCodeViewer viewer(false);
    viewer.load_for_capture(result, print, capture);
    Camera camera;
    // The processor has already applied this plate's display offset to the G-code vertices.
    camera.restore_view(capture, source_plate.get_origin());
    BoundingBoxf3 scene = viewer.capture_bounding_box(capture.dynapin);
    Pointfs       shape, excluded;
    const Vec3d   origin = source_plate.get_origin();
    for (const auto& p : source_plate.get_shape())
        shape.emplace_back(p - origin.head<2>());
    for (const auto& p : source_plate.get_exclude_shape())
        excluded.emplace_back(p - origin.head<2>());
    for (const auto& p : shape)
        scene.merge(Vec3d(p.x() + origin.x(), p.y() + origin.y(), 0.0));

    // Use a capture-owned plate: no GPU handles can escape into a different context.
    PartPlate plate(nullptr, origin, 1, 1, 1, nullptr, nullptr, true, ptFFF);
    plate.set_shape(shape, excluded, {}, {}, origin.head<2>(), 0, 0);
    Bed3D             bed;
    GLTexture         texture;
    const std::string model_file = resolve_asset(capture.bed_model);
    if (capture.bed && !model_file.empty() && !boost::filesystem::is_regular_file(model_file))
        throw std::runtime_error("Cannot load preview bed model: " + model_file);
    if (capture.bed) {
        bed.on_change_color_mode(capture.dark);
        bed.set_shape(shape, print.config().printable_height.value, {}, {}, model_file, true, origin.head<2>());
        bed.set_axes_mode(false);
        scene.merge(bed.capture_bounding_box());
    }
    for (const auto& part : capture.bed_parts) {
        const auto& r = part.rectangle;
        scene.merge(Vec3d(origin.x() + r[0], origin.y() + r[1], 0));
        scene.merge(Vec3d(origin.x() + r[0] + r[2], origin.y() + r[1] + r[3], 0));
    }
    camera.set_scene_box(scene);
    camera.apply_viewport();
    camera.apply_projection(scene, -1.0, -1.0, true);
    if (capture.bed) {
        bed.render_capture(camera, gl.get_shader("flat"), gl.get_shader("hotbed"), capture.axes);
        const auto texture_file = resolve_asset(capture.bed_texture);
        GLTexture* logo         = nullptr;
        if (!texture_file.empty()) {
            load_capture_texture(texture, texture_file);
            logo = &texture;
        }
#if SLIC3R_OPENGL_ES
        auto* lines = gl.get_shader("dashed_lines");
#else
        auto* lines = gl.get_shader(OpenGLManager::get_gl_info().is_core_profile() ? "dashed_thick_lines" : "flat");
#endif
        if (capture.grid && !lines)
            throw std::runtime_error("Preview capture grid shader is unavailable");
        std::vector<std::unique_ptr<GLTexture>>      textures;
        std::vector<std::unique_ptr<GLModel>>        models;
        std::vector<std::pair<GLTexture*, GLModel*>> parts;
        for (const auto& item : capture.bed_parts) {
            const auto& rect         = item.rectangle;
            auto        part_texture = std::make_unique<GLTexture>();
            load_capture_texture(*part_texture, resolve_asset(item.file));
            PartPlateList::BedTextureInfo::TexturePart part(rect[0], rect[1], rect[2], rect[3], item.file);
            part.offset = origin.head<2>();
            part.update_buffer();
            std::unique_ptr<GLModel> model(part.buffer);
            if (model) {
                parts.emplace_back(part_texture.get(), model.get());
                textures.push_back(std::move(part_texture));
                models.push_back(std::move(model));
            }
        }
        plate.render_capture(camera, gl.get_shader("flat"), lines, gl.get_shader("printbed"), logo, capture.dark, capture.grid, parts);
    }
    viewer.render_capture(camera, gl.get_shader("gouraud_light"), capture.dynapin);
    ThumbnailData image;
    image.set(capture.width, capture.height);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, capture.width, capture.height, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels.data());
    // The existing thumbnail encoder flips OpenGL's bottom-up rows.
    const auto png = GCodeThumbnails::compress_thumbnail(image, GCodeThumbnailsFormat::PNG);
    if (!png || !png->data)
        throw std::runtime_error("Cannot encode preview PNG");
    const std::string temporary = path + ".tmp";
    {
        boost::nowide::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(static_cast<const char*>(png->data), png->size);
        stream.close();
        if (!stream) {
            boost::filesystem::remove(temporary);
            throw std::runtime_error("Cannot write preview PNG: " + path);
        }
    }
    if (Slic3r::rename_file(temporary, path)) {
        boost::filesystem::remove(temporary);
        throw std::runtime_error("Cannot replace preview PNG: " + path);
    }
}

void export_preview_image_cli(
    const std::string& path, const PreviewCapture& capture, const Print& print, const GCodeProcessorResult& result, PartPlate& plate)
{
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG))
        wxImage::AddHandler(new wxPNGHandler);
    HiddenCaptureContext context;
    OpenGLManager        gl;
    if (!gl.init_gl(false))
        throw std::runtime_error("Cannot initialize preview capture OpenGL shaders");
    export_preview_image(path, capture, print, result, plate, gl);
}
} // namespace Slic3r::GUI
