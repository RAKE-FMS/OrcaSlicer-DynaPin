#ifndef slic3r_PreviewCaptureDialog_hpp_
#define slic3r_PreviewCaptureDialog_hpp_

namespace Slic3r::GUI {
class GLCanvas3D;
class PartPlate;
void show_preview_capture_dialog(GLCanvas3D& canvas, PartPlate& plate);
// Call before plate identities or undo/redo state are replaced.
void close_preview_capture_dialog();
} // namespace Slic3r::GUI
#endif
