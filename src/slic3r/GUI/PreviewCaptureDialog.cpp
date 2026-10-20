#include "PreviewCaptureDialog.hpp"
#include "PreviewCaptureRenderer.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "Camera.hpp"
#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <algorithm>
#include <cmath>

namespace Slic3r::GUI {
namespace {
class CaptureImagePanel : public wxPanel
{
    wxImage m_image;

public:
    explicit CaptureImagePanel(wxWindow* parent) : wxPanel(parent)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(360, 260)));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour()));
            dc.Clear();
            if (!m_image.IsOk())
                return;
            const auto   size   = GetClientSize();
            const double fit    = std::min(double(size.x) / m_image.GetWidth(), double(size.y) / m_image.GetHeight());
            const int    width  = std::max(1, int(m_image.GetWidth() * fit));
            const int    height = std::max(1, int(m_image.GetHeight() * fit));
            const double scale  = GetContentScaleFactor();
            wxBitmap     bitmap(m_image.Scale(std::max(1, int(width * scale)), std::max(1, int(height * scale)), wxIMAGE_QUALITY_HIGH), -1,
                                scale);
            dc.DrawBitmap(bitmap, (size.x - width) / 2, (size.y - height) / 2);
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            Refresh();
            event.Skip();
        });
    }
    void set_image(const ThumbnailData& pixels)
    {
        m_image.Create(pixels.width, pixels.height);
        auto* rgb = m_image.GetData();
        for (unsigned y = 0; y < pixels.height; ++y)
            for (unsigned x = 0; x < pixels.width; ++x) {
                const size_t src = (size_t(pixels.height - 1 - y) * pixels.width + x) * 4;
                const size_t dst = (size_t(y) * pixels.width + x) * 3;
                std::copy_n(pixels.pixels.data() + src, 3, rgb + dst);
            }
        Refresh();
    }
    void clear_image()
    {
        m_image.Destroy();
        Refresh();
    }
};
} // namespace

class PreviewCaptureDialog : public DPIDialog
{
    GLCanvas3D*                   m_canvas;
    PartPlate&                    m_plate;
    CaptureImagePanel*            m_image;
    wxStaticText*                 m_status;
    wxStaticText*                 m_dimensions;
    wxChoice*                     m_ratio;
    wxChoice*                     m_scene;
    wxSpinCtrl*                   m_width;
    wxSpinCtrl*                   m_height;
    wxButton*                     m_apply;
    wxButton*                     m_record;
    wxButton*                     m_restore;
    wxButton*                     m_delete;
    wxButton*                     m_export;
    wxTimer                       m_timer;
    std::optional<PreviewCapture> m_draft;
    std::string                   m_saved;
    double                        m_original_ratio = 1;
    bool                          m_refresh        = true;
    bool                          m_closing        = false;
    bool                          m_width_anchor   = false;
    unsigned int                  m_result_id      = 0;
    size_t                        m_scene_revision = 0;

    void status(const wxString& message)
    {
        m_status->SetLabel(message);
        m_status->SetToolTip(message);
    }
    bool unsliced() const { return m_scene->GetSelection() == 0; }
    bool ready() const { return !busy() && (unsliced() ? !m_plate.empty() : m_plate.is_slice_result_valid()); }
    bool busy() const
    { return wxGetApp().plater()->is_background_process_slicing() || wxGetApp().plater()->is_background_process_running(); }
    std::string saved_settings() const
    { return m_plate.preview_capture() ? serialize_preview_captures({{0, *m_plate.preview_capture()}}) : std::string(); }
    void load_settings()
    {
        m_draft = m_plate.preview_capture();
        m_saved = saved_settings();
        m_ratio->SetSelection(0);
        if (m_draft) {
            m_original_ratio = double(m_draft->width) / m_draft->height;
            m_width->SetValue(m_draft->width);
            m_height->SetValue(m_draft->height);
        } else {
            m_image->clear_image();
        }
        m_refresh = true;
        update_controls();
    }
    void update_controls()
    {
        const bool exists     = m_draft.has_value();
        const bool processing = busy();
        const bool valid      = exists && ready();
        m_ratio->Enable(exists);
        m_width->Enable(exists);
        m_height->Enable(exists);
        m_apply->Enable(exists && serialize_preview_captures({{0, *m_draft}}) != m_saved);
        const auto type = wxGetApp().plater()->get_current_canvas3D()->get_canvas_type();
        m_record->Enable(!processing && wxGetApp().plater()->get_partplate_list().get_curr_plate() == &m_plate && !m_plate.empty() &&
                         (type == GLCanvas3D::ECanvasType::CanvasView3D || type == GLCanvas3D::ECanvasType::CanvasPreview));
        m_restore->Enable(m_plate.preview_capture().has_value() && !processing);
        m_delete->Enable(m_plate.preview_capture().has_value());
        m_export->Enable(valid);
        m_dimensions->SetLabel(exists ? wxString::Format("%u × %u px", m_draft->width, m_draft->height) : wxString());
    }
    double ratio() const
    {
        switch (m_ratio->GetSelection()) {
        case 0: return m_original_ratio;
        case 1: return 1.0;
        case 2: return 4.0 / 3;
        case 3: return 3.0 / 2;
        case 4: return 16.0 / 9;
        case 5: return 9.0 / 16;
        default: return 0;
        }
    }
    bool update_size(bool width_changed, bool aspect_changed = false)
    {
        if (m_closing || !m_draft)
            return false;
        unsigned int width = m_width->GetValue(), height = m_height->GetValue();
        if (!aspect_changed && width == m_draft->width && height == m_draft->height)
            return true;
        m_width_anchor      = width_changed;
        const double aspect = ratio();
        if (aspect > 0) {
            if (width_changed)
                height = std::max(1L, std::lround(width / aspect));
            else
                width = std::max(1L, std::lround(height * aspect));
        }
        try {
            auto resized = m_draft->resized(width, height);
            if (resized.width != m_draft->width || resized.height != m_draft->height)
                m_refresh = true;
            m_draft = std::move(resized);
            m_width->SetValue(width);
            m_height->SetValue(height);
            update_controls();
            return true;
        } catch (const std::exception& error) {
            status(from_u8(error.what()));
            return false;
        }
    }
    void tick()
    {
        if (m_closing)
            return;
        // Structural changes close this window synchronously before replacing the plates.
        if (saved_settings() != m_saved) {
            Close();
            return;
        }
        update_controls();
        if (!m_draft) {
            status(_L("No recorded view. Record the current view to begin."));
            return;
        }
        if (!ready()) {
            status(busy()     ? _L("Waiting for background processing. The displayed image may be out of date.") :
                   unsliced() ? _L("No printable models to capture. The displayed image may be out of date.") :
                                _L("Waiting for a valid slice. The displayed image may be out of date."));
            m_refresh = true;
            return;
        }
        const auto* result = m_plate.get_slice_result();
        const size_t revision = wxGetApp().plater()->get_view3D_canvas3D()->m_capture_scene_revision;
        if (!m_refresh && (unsliced() ? m_scene_revision == revision : (result && m_result_id == result->id)))
            return;
        m_refresh   = false;
        m_scene_revision = revision;
        m_result_id = result ? result->id : 0;
        try {
            const double scale   = std::min({1.0, 1600.0 / m_draft->width, 1600.0 / m_draft->height});
            const auto   capture = m_draft->resized(std::max(1U, unsigned(std::lround(m_draft->width * scale))),
                                                    std::max(1U, unsigned(std::lround(m_draft->height * scale))));
            m_image->set_image(m_canvas->render_preview_capture(capture, m_plate, unsliced()));
            status(unsliced() ? _L("Unsliced model from the recorded view") : _L("Sliced toolpaths from the recorded view"));
        } catch (const std::exception& error) {
            status(_L("Preview capture failed") + ": " + from_u8(error.what()));
        }
        Layout();
    }
    void apply()
    {
        if (!update_size(m_width_anchor))
            return;
        if (serialize_preview_captures({{0, *m_draft}}) == m_saved)
            return;
        auto* plater = wxGetApp().plater();
        plater->take_snapshot(_u8L("Change preview capture size"), UndoRedo::SnapshotType::PreviewCapture);
        m_plate.set_preview_capture(*m_draft);
        m_saved = saved_settings();
        plater->set_plater_dirty(true);
        m_canvas->set_as_dirty();
        update_controls();
    }
    void export_png()
    {
        if (!update_size(m_width_anchor) || !ready())
            return;
        wxWeakRef<PreviewCaptureDialog> alive(this);
        wxFileDialog dialog(wxGetApp().plater(), _L("Save preview image"), wxEmptyString, unsliced() ? "unsliced.png" : "sliced.png",
                            "PNG (*.png)|*.png", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        const int    response = dialog.ShowModal();
        if (!alive || alive->m_closing || response != wxID_OK)
            return;
        if (!ready())
            return;
        try {
            save_preview_image(into_u8(dialog.GetPath()), m_canvas->render_preview_capture(*m_draft, m_plate, unsliced()));
            status(_L("Preview image saved"));
        } catch (const std::exception& error) {
            status(_L("Preview capture failed") + ": " + from_u8(error.what()));
        }
    }
    void on_dpi_changed(const wxRect&) override
    {
        Layout();
        Refresh();
    }

public:
    void show_scene(GLCanvas3D& canvas)
    {
        m_canvas = &canvas;
        m_scene->SetSelection(canvas.get_canvas_type() == GLCanvas3D::ECanvasType::CanvasView3D ? 0 : 1);
        m_refresh = true;
        Raise();
    }
    bool targets(const PartPlate& plate) const { return &m_plate == &plate; }
    PreviewCaptureDialog(GLCanvas3D& canvas, PartPlate& plate)
        : DPIDialog(wxGetApp().plater(),
                    wxID_ANY,
                    _L("Capture settings") + " — " +
                        (plate.get_plate_name().empty() ? wxString::Format(_L("Plate %d"), plate.get_index() + 1) :
                                                          from_u8(plate.get_plate_name())),
                    wxDefaultPosition,
                    wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_canvas(&canvas)
        , m_plate(plate)
        , m_timer(this)
    {
        SetFont(wxGetApp().normal_font());
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* body = new wxBoxSizer(wxHORIZONTAL);
        auto* left = new wxBoxSizer(wxVERTICAL);
        m_image    = new CaptureImagePanel(this);
        left->Add(m_image, 1, wxEXPAND);
        m_status = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                    wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        m_status->SetMinSize(wxSize(1, FromDIP(22)));
        left->Add(m_status, 0, wxEXPAND | wxTOP, FromDIP(8));
        body->Add(left, 1, wxEXPAND | wxALL, FromDIP(12));
        auto* controls = new wxBoxSizer(wxVERTICAL);
        controls->Add(new wxStaticText(this, wxID_ANY, _L("Image")), 0, wxBOTTOM, FromDIP(6));
        m_scene = new wxChoice(this, wxID_ANY);
        m_scene->Append(_L("Unsliced model"));
        m_scene->Append(_L("Sliced toolpaths"));
        m_scene->SetSelection(canvas.get_canvas_type() == GLCanvas3D::ECanvasType::CanvasView3D ? 0 : 1);
        m_scene->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            m_refresh = true;
            update_controls();
        });
        controls->Add(m_scene, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
        controls->Add(new wxStaticText(this, wxID_ANY, _L("Aspect ratio")), 0, wxBOTTOM, FromDIP(6));
        m_ratio = new wxChoice(this, wxID_ANY);
        for (const auto& label : std::array<wxString, 7>{_L("Recorded ratio"), "1:1", "4:3", "3:2", "16:9", "9:16", _L("Free")})
            m_ratio->Append(label);
        controls->Add(m_ratio, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
        auto add_size = [&](const wxString& label) {
            controls->Add(new wxStaticText(this, wxID_ANY, label), 0, wxBOTTOM, FromDIP(6));
            auto* spin = new wxSpinCtrl(this, wxID_ANY, "1", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER, 1,
                                        16384, 1);
            controls->Add(spin, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
            return spin;
        };
        m_width      = add_size(_L("Width (px)"));
        m_height     = add_size(_L("Height (px)"));
        m_dimensions = new wxStaticText(this, wxID_ANY, wxEmptyString);
        controls->Add(m_dimensions, 0, wxBOTTOM, FromDIP(12));
        auto button = [&](const wxString& label, auto action) {
            auto* result = new wxButton(this, wxID_ANY, label);
            result->Bind(wxEVT_BUTTON, action);
            controls->Add(result, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
            return result;
        };
        m_record  = button(_L("Record current view"), [this](wxCommandEvent&) {
            if (wxGetApp().plater()->get_partplate_list().get_curr_plate() != &m_plate)
                return;
            wxGetApp().plater()->get_current_canvas3D()->_record_preview_capture();
            load_settings();
        });
        m_restore = button(_L("Restore recorded view"), [this](wxCommandEvent&) {
            wxGetApp().plater()->select_plate(m_plate.get_index());
            m_canvas->_restore_preview_capture();
        });
        m_delete  = button(_L("Delete recorded view"), [this](wxCommandEvent&) {
            auto* plater = wxGetApp().plater();
            plater->take_snapshot(_u8L("Delete preview capture view"), UndoRedo::SnapshotType::PreviewCapture);
            m_plate.set_preview_capture(std::nullopt);
            plater->set_plater_dirty(true);
            m_canvas->set_as_dirty();
            load_settings();
        });
        m_export  = button(_L("Export preview PNG"), [this](wxCommandEvent&) { export_png(); });
        body->Add(controls, 0, wxEXPAND | wxALL, FromDIP(12));
        root->Add(body, 1, wxEXPAND);
        auto* footer = new wxBoxSizer(wxHORIZONTAL);
        footer->AddStretchSpacer();
        m_apply = new wxButton(this, wxID_APPLY, _L("Apply"));
        m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply(); });
        footer->Add(m_apply, 0, wxRIGHT, FromDIP(8));
        auto* close = new wxButton(this, wxID_CLOSE, _L("Close"));
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
        footer->Add(close);
        root->Add(footer, 0, wxEXPAND | wxALL, FromDIP(12));
        SetSizer(root);
        SetSize(FromDIP(wxSize(960, 600)));
        SetMinSize(FromDIP(wxSize(720, 480)));
        CentreOnParent();
        m_ratio->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { update_size(false, true); });
        auto bind_size = [this](wxSpinCtrl* spin, bool width) {
            spin->Bind(wxEVT_SPINCTRL, [this, width](wxSpinEvent&) { update_size(width); });
            spin->Bind(wxEVT_TEXT_ENTER, [this, width](wxCommandEvent&) { update_size(width); });
            spin->Bind(wxEVT_KILL_FOCUS, [this, width](wxFocusEvent& event) {
                update_size(width);
                event.Skip();
            });
        };
        bind_size(m_width, true);
        bind_size(m_height, false);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
            m_closing = true;
            m_timer.Stop();
            Hide();
            Destroy();
        });
        wxGetApp().UpdateDarkUI(this);
        wxGetApp().UpdateDlgDarkUI(this);
        load_settings();
        m_timer.Start(250);
    }
};

namespace {
wxWeakRef<PreviewCaptureDialog> capture_dialog;
}
void close_preview_capture_dialog()
{
    if (capture_dialog) {
        capture_dialog->Close();
        capture_dialog = nullptr;
    }
}
void show_preview_capture_dialog(GLCanvas3D& canvas, PartPlate& plate)
{
    if (capture_dialog && capture_dialog->targets(plate)) {
        capture_dialog->show_scene(canvas);
        return;
    }
    close_preview_capture_dialog();
    auto* dialog   = new PreviewCaptureDialog(canvas, plate);
    capture_dialog = dialog;
    dialog->Show();
}
} // namespace Slic3r::GUI
