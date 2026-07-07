#pragma once

#include <wx/config.h>
#include <wx/filedlg.h>
#include <wx/grid.h>
#include <wx/settings.h>
#include <wx/statusbr.h>
#include <wx/dcbuffer.h>
#include <wx/textdlg.h>
#include <wx/wx.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wxui {

inline wxString toWx(const std::string& text) {
    return wxString::FromUTF8(text.c_str());
}

inline std::string toStd(const wxString& text) {
    const wxScopedCharBuffer buffer = text.ToUTF8();
    return buffer ? std::string(buffer.data()) : std::string();
}

struct ThemePalette {
    wxColour frame;
    wxColour panel;
    wxColour field;
    wxColour fieldAlt;
    wxColour button;
    wxColour text;
    wxColour gridLine;
    wxColour selection;
    wxColour selectionText;
};

inline bool& activeDarkMode() {
    static bool enabled = false;
    return enabled;
}

inline bool currentDarkMode() {
    return activeDarkMode();
}

inline ThemePalette themePalette(bool darkMode) {
    if (darkMode) {
        return ThemePalette{
            wxColour(24, 24, 27),
            wxColour(32, 33, 36),
            wxColour(18, 18, 21),
            wxColour(33, 33, 36),
            wxColour(44, 44, 48),
            wxColour(238, 238, 238),
            wxColour(72, 72, 78),
            wxColour(65, 105, 160),
            wxColour(255, 255, 255)
        };
    }
    const wxColour face = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
    return ThemePalette{
        face,
        face,
        wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW),
        wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW),
        face,
        wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT),
        wxSystemSettings::GetColour(wxSYS_COLOUR_3DLIGHT),
        wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT),
        wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT)
    };
}


class ThemedStatusBar final : public wxStatusBar {
public:
    explicit ThemedStatusBar(wxWindow* parent, int fields = 1)
        : wxStatusBar(parent, wxID_ANY, wxSTB_DEFAULT_STYLE | wxFULL_REPAINT_ON_RESIZE) {
        const int safeFields = std::max(1, fields);
        wxStatusBar::SetFieldsCount(safeFields);
        labels_.assign(static_cast<std::size_t>(safeFields), wxString{});
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &ThemedStatusBar::onPaint, this);
        Bind(wxEVT_ERASE_BACKGROUND, &ThemedStatusBar::onEraseBackground, this);
        Bind(wxEVT_SIZE, &ThemedStatusBar::onSize, this);
        applyPalette();
    }

    void setDarkMode(bool enabled) {
        darkMode_ = enabled;
        applyPalette();
        Refresh(false);
        Update();
    }

    void SetStatusText(const wxString& text, int number = 0) {
        if (number < 0) {
            return;
        }
        ensureLabelCount(number + 1);
        labels_[static_cast<std::size_t>(number)] = text;

        // Keep native status-bar text empty.  On MSW the native status-bar
        // control can repaint labels using the system foreground after the
        // theme pass, which produces black footer text in dark mode.
        wxStatusBar::SetStatusText(wxString{}, number);
        Refresh(false);
        Update();
    }

private:
    void ensureLabelCount(int minimum) {
        if (minimum <= 0) {
            return;
        }
        if (minimum > GetFieldsCount()) {
            wxStatusBar::SetFieldsCount(minimum);
        }
        if (static_cast<std::size_t>(minimum) > labels_.size()) {
            labels_.resize(static_cast<std::size_t>(minimum));
        }
    }

    void applyPalette() {
        const ThemePalette palette = themePalette(darkMode_);
        SetBackgroundColour(palette.button);
        SetForegroundColour(palette.text);
    }

    void onEraseBackground(wxEraseEvent&) {
        // The themed status bar is fully painted in onPaint().
    }

    void onSize(wxSizeEvent& event) {
        event.Skip();
        Refresh(false);
    }

    void onPaint(wxPaintEvent&) {
        const ThemePalette palette = themePalette(darkMode_);
        wxAutoBufferedPaintDC dc(this);
        const wxRect client = GetClientRect();

        dc.SetBackground(wxBrush(palette.button));
        dc.Clear();
        dc.SetPen(wxPen(palette.button));
        dc.SetBrush(wxBrush(palette.button));
        dc.DrawRectangle(client.GetX(), client.GetY(), client.GetWidth(), client.GetHeight());

        dc.SetFont(GetFont());
        dc.SetTextForeground(palette.text);
        dc.SetPen(wxPen(palette.gridLine));
        if (client.GetHeight() > 1) {
            dc.DrawLine(client.GetLeft(), client.GetTop(), client.GetRight(), client.GetTop());
        }

        const int fields = std::max(1, GetFieldsCount());
        ensureLabelCount(fields);
        for (int field = 0; field < fields; ++field) {
            wxRect rect;
            if (!GetFieldRect(field, rect)) {
                continue;
            }

            if (field > 0) {
                dc.SetPen(wxPen(palette.gridLine));
                dc.DrawLine(rect.GetLeft(), rect.GetTop() + FromDIP(3),
                            rect.GetLeft(), rect.GetBottom() - FromDIP(3));
            }

            const wxString& text = labels_[static_cast<std::size_t>(field)];
            if (text.empty()) {
                continue;
            }

            wxRect textRect = rect;
            textRect.Deflate(FromDIP(6), 0);
            const wxSize extent = dc.GetTextExtent(text);
            int y = textRect.GetTop() + std::max(0, (textRect.GetHeight() - extent.GetHeight()) / 2);
            if (y < textRect.GetTop()) {
                y = textRect.GetTop();
            }

            dc.SetClippingRegion(textRect);
            dc.DrawText(text, textRect.GetLeft(), y);
            dc.DestroyClippingRegion();
        }
    }

    bool darkMode_ = false;
    std::vector<wxString> labels_;
};

inline wxStatusBar* createStatusBar(wxFrame& frame, int fields = 1) {
    auto* status = new ThemedStatusBar(&frame, fields);
    frame.SetStatusBar(status);
    status->setDarkMode(currentDarkMode());
    return status;
}

inline void applyStatusBarTheme(wxStatusBar& status, bool darkMode) {
    if (auto* themed = dynamic_cast<ThemedStatusBar*>(&status)) {
        themed->setDarkMode(darkMode);
    } else {
        const ThemePalette palette = themePalette(darkMode);
        status.SetBackgroundColour(palette.button);
        status.SetForegroundColour(palette.text);
        status.Refresh(false);
        status.Update();
    }
}

inline void setStatusText(wxFrame& frame, const wxString& text, int field = 0) {
    if (auto* status = frame.GetStatusBar()) {
        if (auto* themed = dynamic_cast<ThemedStatusBar*>(status)) {
            themed->SetStatusText(text, field);
            return;
        }
    }
    frame.SetStatusText(text, field);
}

inline void setStatusText(wxFrame& frame, const std::string& text, int field = 0) {
    setStatusText(frame, toWx(text), field);
}

inline void setStatusText(wxFrame& frame, const char* text, int field = 0) {
    setStatusText(frame, wxString::FromUTF8(text), field);
}

inline bool readDarkMode(const std::string& appName) {
    wxConfig config(toWx(appName));
    bool enabled = false;
    config.Read("DarkMode", &enabled, false);
    return enabled;
}

inline void writeDarkMode(const std::string& appName, bool enabled) {
    wxConfig config(toWx(appName));
    config.Write("DarkMode", enabled);
    config.Flush();
}

inline void applyGridTheme(wxGrid& grid, bool darkMode) {
    const ThemePalette palette = themePalette(darkMode);
    grid.SetBackgroundColour(palette.field);
    grid.SetForegroundColour(palette.text);
    grid.SetDefaultCellBackgroundColour(palette.field);
    grid.SetDefaultCellTextColour(palette.text);
    grid.SetLabelBackgroundColour(darkMode ? palette.panel : wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
    grid.SetLabelTextColour(palette.text);
    grid.SetGridLineColour(palette.gridLine);
    grid.SetSelectionBackground(palette.selection);
    grid.SetSelectionForeground(palette.selectionText);
    for (int row = 0; row < grid.GetNumberRows(); ++row) {
        for (int col = 0; col < grid.GetNumberCols(); ++col) {
            grid.SetCellBackgroundColour(row, col, darkMode && (row % 2) ? palette.fieldAlt : palette.field);
            grid.SetCellTextColour(row, col, palette.text);
        }
    }
    grid.ForceRefresh();
}

inline void applyTheme(wxWindow* window, bool darkMode) {
    activeDarkMode() = darkMode;
    if (window == nullptr) {
        return;
    }

    const ThemePalette palette = themePalette(darkMode);
    wxColour background = palette.panel;
    if (dynamic_cast<wxFrame*>(window) != nullptr || dynamic_cast<wxDialog*>(window) != nullptr) {
        background = palette.frame;
    }
    if (dynamic_cast<wxPanel*>(window) != nullptr) {
        background = palette.panel;
    }
    if (dynamic_cast<wxTextCtrl*>(window) != nullptr) {
        background = palette.field;
    }
    if (dynamic_cast<wxButton*>(window) != nullptr) {
        background = palette.button;
    }
    window->SetBackgroundColour(background);
    window->SetForegroundColour(palette.text);

    if (auto* grid = dynamic_cast<wxGrid*>(window)) {
        applyGridTheme(*grid, darkMode);
    }
    if (auto* status = dynamic_cast<wxStatusBar*>(window)) {
        applyStatusBarTheme(*status, darkMode);
    }
    if (auto* frame = dynamic_cast<wxFrame*>(window)) {
        if (auto* status = frame->GetStatusBar()) {
            applyStatusBarTheme(*status, darkMode);
        }
    }

    for (wxWindowList::compatibility_iterator node = window->GetChildren().GetFirst(); node; node = node->GetNext()) {
        applyTheme(node->GetData(), darkMode);
    }
    window->Refresh();
}

inline void showError(wxWindow* parent, const std::exception& ex) {
    wxMessageBox(toWx(ex.what()), "Error", wxOK | wxICON_ERROR, parent);
}

inline void showMessage(wxWindow* parent, const std::string& title, const std::string& message) {
    wxMessageBox(toWx(message), toWx(title), wxOK | wxICON_INFORMATION, parent);
}

inline bool confirm(wxWindow* parent, const std::string& title, const std::string& message) {
    return wxMessageBox(toWx(message), toWx(title), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, parent) == wxYES;
}

inline std::optional<std::string> promptText(wxWindow* parent,
                                             const std::string& title,
                                             const std::string& message,
                                             const std::string& defaultValue = {}) {
    wxTextEntryDialog dialog(parent, toWx(message), toWx(title), toWx(defaultValue));
    applyTheme(&dialog, currentDarkMode());
    if (dialog.ShowModal() != wxID_OK) {
        return std::nullopt;
    }
    return toStd(dialog.GetValue());
}

inline std::optional<std::filesystem::path> chooseOpenFile(wxWindow* parent,
                                                           const std::string& title,
                                                           const std::string& wildcard) {
    wxFileDialog dialog(parent, toWx(title), wxEmptyString, wxEmptyString, toWx(wildcard),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) {
        return std::nullopt;
    }
    return std::filesystem::path(toStd(dialog.GetPath()));
}

inline std::optional<std::filesystem::path> chooseSaveFile(wxWindow* parent,
                                                           const std::string& title,
                                                           const std::string& wildcard,
                                                           const std::string& defaultFile = {}) {
    wxFileDialog dialog(parent, toWx(title), wxEmptyString, toWx(defaultFile), toWx(wildcard),
                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK) {
        return std::nullopt;
    }
    return std::filesystem::path(toStd(dialog.GetPath()));
}

} // namespace wxui
