#include "core/TwoDAFile.hpp"
#include "core/Version.hpp"
#include "wx_ui.hpp"
#include "neo2da_icon.xpm"
#include "TabularData.hpp"
#include "TslPatcher.hpp"

#include <wx/clipbrd.h>
#include <wx/dir.h>
#include <wx/dirdlg.h>
#include <wx/grid.h>
#include <wx/icon.h>
#include <wx/iconbndl.h>
#include <wx/sizer.h>
#include <wx/wx.h>
#include <wx/version.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace neo2da;

constexpr const char* k2DAWildcard =
    "All supported 2DA tables (*.2da;*.gda)|*.2da;*.gda|"
    "KotOR/Infinity-style 2DA files (*.2da)|*.2da|"
    "Dragon Age GDA files (*.gda)|*.gda|"
    "All files (*.*)|*.*";
constexpr const char* kCsvWildcard = "CSV files (*.csv)|*.csv";
constexpr const char* kTsvWildcard = "TSV files (*.tsv)|*.tsv";

const char* wildcardForFlatFormat(neotabular::Format format) {
    switch (format) {
    case neotabular::Format::Csv: return kCsvWildcard;
    case neotabular::Format::Tsv: return kTsvWildcard;
    default: throw std::runtime_error("Neo2DA only supports CSV and TSV table import/export.");
    }
}

std::string exportExtensionForFormat(neotabular::Format format) {
    switch (format) {
    case neotabular::Format::Csv: return "csv";
    case neotabular::Format::Tsv: return "tsv";
    }
    return "txt";
}

std::string exportDefaultFilename(const std::filesystem::path& source,
                                  neotabular::Format format,
                                  const std::string& fallbackStem) {
    std::string stem = source.empty() ? fallbackStem : source.stem().string();
    if (stem.empty()) stem = fallbackStem.empty() ? std::string("export") : fallbackStem;
    return stem + "." + exportExtensionForFormat(format);
}

std::string pathText(const std::filesystem::path& path) {
    return path.empty() ? std::string{} : path.string();
}

enum : int {
    ID_New = wxID_HIGHEST + 1,
    ID_Open,
    ID_Save,
    ID_SaveAs,
    ID_AddRow,
    ID_AddColumn,
    ID_CloneRow,
    ID_DeleteRow,
    ID_DeleteColumn,
    ID_RenameRow,
    ID_RenameColumn,
    ID_CopyCells,
    ID_PasteCells,
    ID_Filter,
    ID_ClearFilter,
    ID_ImportCsv,
    ID_ImportTsv,
    ID_ExportCsv,
    ID_ExportTsv,
    ID_GeneratePatcherPackage,
    ID_GeneratePatcherFragment,
    ID_DarkMode,
    ID_Grid
};

class Neo2DAFrame final : public wxFrame {
public:
    Neo2DAFrame()
        : wxFrame(nullptr, wxID_ANY, wxui::toWx(std::string("Neo2DA v") + kNeo2DAVersion + " (2DA table editor)"), wxDefaultPosition, wxDefaultSize) {
        setApplicationIcon();
        buildMenus();
        buildMainWindow();
        wxui::createStatusBar(*this, 2);
        darkMode_ = wxui::readDarkMode("Neo2DA");
        applyDarkMode();
        SetMinSize(FromDIP(wxSize(760, 500)));
        SetInitialSize(FromDIP(wxSize(1050, 720)));
        newTable(false);
    }

    void openStartupFile(const std::filesystem::path& path) {
        if (path.empty()) {
            return;
        }
        try {
            table_.load(path);
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

private:
    void setApplicationIcon() {
        wxIconBundle bundle;
#if defined(__WXMSW__)
        wxIcon windowsIcon("neo2da", wxBITMAP_TYPE_ICO_RESOURCE);
        if (windowsIcon.IsOk()) {
            bundle.AddIcon(windowsIcon);
        }
#endif
        wxIcon fallbackIcon(neo2da_icon_xpm);
        if (fallbackIcon.IsOk()) {
            bundle.AddIcon(fallbackIcon);
        }
        if (bundle.GetIconCount() > 0) {
            SetIcons(bundle);
        }
    }

    void buildMenus() {
        auto* file = new wxMenu;
        file->Append(ID_New, "&New 2DA");
        file->Append(ID_Open, "&Open 2DA/GDA...");
        file->Append(ID_Save, "&Save");
        file->Append(ID_SaveAs, "Save &As...");
        file->AppendSeparator();
        file->Append(wxID_EXIT, "E&xit");

        auto* import = new wxMenu;
        import->Append(ID_ImportCsv, "Import &CSV...");
        import->Append(ID_ImportTsv, "Import &TSV...");

        auto* exportMenu = new wxMenu;
        exportMenu->Append(ID_ExportCsv, "Export as &CSV...");
        exportMenu->Append(ID_ExportTsv, "Export as &TSV...");

        auto* tools = new wxMenu;
        tools->Append(ID_GeneratePatcherPackage, "Generate TSLPatcher/HoloPatcher &Package...");
        tools->Append(ID_GeneratePatcherFragment, "Generate TSLPatcher/HoloPatcher INI &Fragment...");

        auto* edit = new wxMenu;
        edit->Append(ID_CopyCells, "&Copy Cells	Ctrl-C");
        edit->Append(ID_PasteCells, "&Paste Cells	Ctrl-V");
        edit->AppendSeparator();
        edit->Append(ID_Filter, "&Filter/Search...	Ctrl-F");
        edit->Append(ID_ClearFilter, "Clear Filter");
        edit->AppendSeparator();
        edit->Append(ID_AddRow, "Add &Row...");
        edit->Append(ID_AddColumn, "Add &Column...");
        edit->Append(ID_CloneRow, "&Clone Selected Row...");
        edit->Append(ID_DeleteRow, "Delete Selected Row");
        edit->Append(ID_DeleteColumn, "Delete Selected Column");
        edit->AppendSeparator();
        edit->Append(ID_RenameRow, "Rename Selected &Row...");
        edit->Append(ID_RenameColumn, "Rename Selected &Column...");

        auto* view = new wxMenu;
        darkModeItem_ = view->AppendCheckItem(ID_DarkMode, "&Dark Mode");

        auto* help = new wxMenu;
        help->Append(wxID_ABOUT, "&About");

        auto* bar = new wxMenuBar;
        bar->Append(file, "&File");
        bar->Append(import, "&Import");
        bar->Append(exportMenu, "&Export");
        bar->Append(tools, "&Tools");
        bar->Append(edit, "&Edit");
        bar->Append(view, "&View");
        bar->Append(help, "&Help");
        SetMenuBar(bar);

        Bind(wxEVT_MENU, &Neo2DAFrame::onNew, this, ID_New);
        Bind(wxEVT_MENU, &Neo2DAFrame::onOpen, this, ID_Open);
        Bind(wxEVT_MENU, &Neo2DAFrame::onSave, this, ID_Save);
        Bind(wxEVT_MENU, &Neo2DAFrame::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_MENU, &Neo2DAFrame::onAddRow, this, ID_AddRow);
        Bind(wxEVT_MENU, &Neo2DAFrame::onAddColumn, this, ID_AddColumn);
        Bind(wxEVT_MENU, &Neo2DAFrame::onCloneRow, this, ID_CloneRow);
        Bind(wxEVT_MENU, &Neo2DAFrame::onDeleteRow, this, ID_DeleteRow);
        Bind(wxEVT_MENU, &Neo2DAFrame::onDeleteColumn, this, ID_DeleteColumn);
        Bind(wxEVT_MENU, &Neo2DAFrame::onRenameRow, this, ID_RenameRow);
        Bind(wxEVT_MENU, &Neo2DAFrame::onRenameColumn, this, ID_RenameColumn);
        Bind(wxEVT_MENU, &Neo2DAFrame::onCopyCells, this, ID_CopyCells);
        Bind(wxEVT_MENU, &Neo2DAFrame::onPasteCells, this, ID_PasteCells);
        Bind(wxEVT_MENU, &Neo2DAFrame::onFilterPrompt, this, ID_Filter);
        Bind(wxEVT_MENU, &Neo2DAFrame::onClearFilter, this, ID_ClearFilter);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(neotabular::Format::Csv); }, ID_ImportCsv);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(neotabular::Format::Tsv); }, ID_ImportTsv);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(neotabular::Format::Csv); }, ID_ExportCsv);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(neotabular::Format::Tsv); }, ID_ExportTsv);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onGeneratePatcherOutput(true); }, ID_GeneratePatcherPackage);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onGeneratePatcherOutput(false); }, ID_GeneratePatcherFragment);
        Bind(wxEVT_MENU, &Neo2DAFrame::onToggleDarkMode, this, ID_DarkMode);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            wxui::showMessage(this, "About Neo2DA", std::string("Neo2DA v") + kNeo2DAVersion + "\nNative wxWidgets 2DA table editor\n\nA special thanks to everyone in the KOTOR modding community that has contributed their work, knowledge, and creativity to making tools, mods, and guides over the last 20+ years");
        }, wxID_ABOUT);
        Bind(wxEVT_BUTTON, &Neo2DAFrame::onOpen, this, ID_Open);
        Bind(wxEVT_BUTTON, &Neo2DAFrame::onSave, this, ID_Save);
        Bind(wxEVT_BUTTON, &Neo2DAFrame::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_BUTTON, &Neo2DAFrame::onAddRow, this, ID_AddRow);
        Bind(wxEVT_BUTTON, &Neo2DAFrame::onAddColumn, this, ID_AddColumn);
        Bind(wxEVT_BUTTON, &Neo2DAFrame::onCloneRow, this, ID_CloneRow);
        Bind(wxEVT_BUTTON, &Neo2DAFrame::onDeleteRow, this, ID_DeleteRow);
        Bind(wxEVT_BUTTON, &Neo2DAFrame::onDeleteColumn, this, ID_DeleteColumn);
        Bind(wxEVT_BUTTON, &Neo2DAFrame::onRenameRow, this, ID_RenameRow);
        Bind(wxEVT_BUTTON, &Neo2DAFrame::onRenameColumn, this, ID_RenameColumn);
        Bind(wxEVT_BUTTON, &Neo2DAFrame::onClearFilter, this, ID_ClearFilter);
        Bind(wxEVT_CLOSE_WINDOW, &Neo2DAFrame::onClose, this);
    }

    void buildMainWindow() {
        auto* panel = new wxPanel(this);
        auto* root = new wxBoxSizer(wxVERTICAL);

        auto* headerBox = new wxStaticBoxSizer(wxVERTICAL, panel, "2DA");

        auto* fileRow = new wxBoxSizer(wxHORIZONTAL);
        fileRow->Add(new wxStaticText(panel, wxID_ANY, "File:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        filePath_ = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        fileRow->Add(filePath_, 1, wxEXPAND | wxRIGHT, FromDIP(6));
        fileRow->Add(new wxButton(panel, ID_Open, "Open..."), 0, wxRIGHT, FromDIP(4));
        fileRow->Add(new wxButton(panel, ID_Save, "Save"), 0, wxRIGHT, FromDIP(4));
        fileRow->Add(new wxButton(panel, ID_SaveAs, "Save As..."), 0);
        headerBox->Add(fileRow, 0, wxEXPAND | wxALL, FromDIP(8));

        auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
        filterRow->Add(new wxStaticText(panel, wxID_ANY, "Filter:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        filterText_ = new wxTextCtrl(panel, wxID_ANY);
        filterRow->Add(filterText_, 1, wxEXPAND | wxRIGHT, FromDIP(4));
        filterRow->Add(new wxButton(panel, ID_ClearFilter, "Clear"), 0);
        headerBox->Add(filterRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        root->Add(headerBox, 0, wxEXPAND | wxALL, FromDIP(8));

        grid_ = new wxGrid(panel, ID_Grid);
        grid_->CreateGrid(0, 0);
        grid_->SetRowLabelSize(95);
        grid_->EnableEditing(true);
        grid_->EnableDragGridSize(false);
        grid_->SetSelectionMode(wxGrid::wxGridSelectCells);
        root->Add(grid_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(panel, ID_AddRow, "Add Row..."), 0, wxRIGHT, 6);
        buttons->Add(new wxButton(panel, ID_AddColumn, "Add Column..."), 0, wxRIGHT, 6);
        buttons->Add(new wxButton(panel, ID_CloneRow, "Clone Row..."), 0, wxRIGHT, 6);
        buttons->Add(new wxButton(panel, ID_DeleteRow, "Delete Row"), 0, wxRIGHT, 6);
        buttons->Add(new wxButton(panel, ID_DeleteColumn, "Delete Column"), 0, wxRIGHT, 6);
        buttons->AddStretchSpacer();
        buttons->Add(new wxButton(panel, ID_RenameRow, "Rename Row..."), 0, wxRIGHT, 6);
        buttons->Add(new wxButton(panel, ID_RenameColumn, "Rename Column..."), 0);
        root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        panel->SetSizer(root);

        filterText_->Bind(wxEVT_TEXT, &Neo2DAFrame::onFilterText, this);
        grid_->Bind(wxEVT_GRID_CELL_CHANGED, &Neo2DAFrame::onCellChanged, this);
        grid_->Bind(wxEVT_GRID_LABEL_LEFT_DCLICK, &Neo2DAFrame::onLabelDoubleClicked, this);
        grid_->Bind(wxEVT_GRID_SELECT_CELL, &Neo2DAFrame::onCellSelected, this);
    }

    void newTable(bool markDirty) {
        table_ = TwoDAFile::create({"Column1"}, 1);
        table_.setFilename({});
        table_.setDirty(markDirty);
        selectedRow_ = 0;
        selectedColumn_ = 0;
        refreshGrid();
        updateStatus();
    }

    bool confirmDiscardIfNeeded() {
        if (!table_.dirty()) {
            return true;
        }
        return wxui::confirm(this, "Discard changes", "The open 2DA has unsaved changes. Continue without saving?");
    }

    std::size_t actualRowForGrid(int gridRow) const {
        if (gridRow < 0 || static_cast<std::size_t>(gridRow) >= visibleRows_.size()) {
            throw TwoDAError("Selected row is outside the current filtered view.");
        }
        return visibleRows_[static_cast<std::size_t>(gridRow)];
    }

    int visibleRowForActual(std::size_t actualRow) const {
        for (std::size_t i = 0; i < visibleRows_.size(); ++i) {
            if (visibleRows_[i] == actualRow) return static_cast<int>(i);
        }
        return -1;
    }

    void rebuildVisibleRows() {
        visibleRows_.clear();
        if (!table_.loaded()) return;
        auto allRows = table_.toTable();
        for (std::size_t row = 0; row < table_.rowCount(); ++row) {
            if (filterTerm_.empty() || (row < allRows.rows.size() && neotabular::rowMatches(allRows, allRows.rows[row], filterTerm_))) {
                visibleRows_.push_back(row);
            }
        }
    }

    void refreshGrid() {
        if (!table_.loaded()) {
            return;
        }
        rebuildVisibleRows();

        const int wantedRows = static_cast<int>(visibleRows_.size());
        const int wantedCols = static_cast<int>(table_.columnCount());
        const int currentRows = grid_->GetNumberRows();
        const int currentCols = grid_->GetNumberCols();
        if (currentRows < wantedRows) {
            grid_->AppendRows(wantedRows - currentRows);
        } else if (currentRows > wantedRows) {
            grid_->DeleteRows(wantedRows, currentRows - wantedRows);
        }
        if (currentCols < wantedCols) {
            grid_->AppendCols(wantedCols - currentCols);
        } else if (currentCols > wantedCols) {
            grid_->DeleteCols(wantedCols, currentCols - wantedCols);
        }

        for (int c = 0; c < wantedCols; ++c) {
            grid_->SetColLabelValue(c, wxui::toWx(table_.columnLabel(static_cast<std::size_t>(c))));
            if (grid_->GetColSize(c) < FromDIP(90)) {
                grid_->SetColSize(c, FromDIP(120));
            }
        }
        for (int r = 0; r < wantedRows; ++r) {
            const std::size_t actualRow = visibleRows_[static_cast<std::size_t>(r)];
            grid_->SetRowLabelValue(r, wxui::toWx(table_.rowLabel(actualRow)));
            for (int c = 0; c < wantedCols; ++c) {
                grid_->SetCellValue(r, c, wxui::toWx(table_.cell(actualRow, static_cast<std::size_t>(c))));
                grid_->SetReadOnly(r, c, false);
            }
        }
        wxui::applyGridTheme(*grid_, darkMode_);
        if (selectedRow_ >= wantedRows) selectedRow_ = wantedRows - 1;
        if (selectedColumn_ >= wantedCols) selectedColumn_ = wantedCols - 1;
        if (selectedRow_ >= 0 && selectedRow_ < wantedRows && selectedColumn_ >= 0 && selectedColumn_ < wantedCols) {
            grid_->SetGridCursor(selectedRow_, selectedColumn_);
            grid_->MakeCellVisible(selectedRow_, selectedColumn_);
        }
    }

    void updateStatus() {
        filePath_->SetValue(wxui::toWx(pathText(table_.filename())));
        const std::string name = table_.filename().empty() ? "Untitled 2DA" : table_.filename().filename().string();
        wxui::setStatusText(*this, wxui::toWx(name + (table_.dirty() ? " modified" : "")), 0);
        if (table_.loaded()) {
            wxui::setStatusText(*this, wxui::toWx(std::to_string(visibleRows_.size()) + "/" +
                                     std::to_string(table_.rowCount()) + " rows, " +
                                     std::to_string(table_.columnCount()) + " columns"), 1);
        } else {
            wxui::setStatusText(*this, "No table", 1);
        }
    }

    void saveTo(const std::filesystem::path& path) {
        table_.save(path);
        table_.setFilename(path);
        table_.setDirty(false);
        updateStatus();
    }

    bool saveAs() {
        const std::string defaultName = table_.filename().empty() ? "new.2da" : table_.filename().filename().string();
        const auto chosen = wxui::chooseSaveFile(this, "Save 2DA/GDA as", k2DAWildcard, defaultName);
        if (!chosen) {
            return false;
        }
        saveTo(*chosen);
        return true;
    }

    int selectedRowOrCursor() const {
        const wxArrayInt rows = grid_->GetSelectedRows();
        if (!rows.IsEmpty()) {
            return rows[0];
        }
        return grid_->GetGridCursorRow();
    }

    int selectedColumnOrCursor() const {
        const wxArrayInt cols = grid_->GetSelectedCols();
        if (!cols.IsEmpty()) {
            return cols[0];
        }
        return grid_->GetGridCursorCol();
    }

    void applyDarkMode() {
        if (darkModeItem_ != nullptr) {
            darkModeItem_->Check(darkMode_);
        }
        wxui::applyTheme(this, darkMode_);
        if (grid_ != nullptr) {
            wxui::applyGridTheme(*grid_, darkMode_);
        }
    }

    void setFilterTerm(std::string term) {
        filterTerm_ = std::move(term);
        if (filterText_ != nullptr && wxui::toStd(filterText_->GetValue()) != filterTerm_) {
            filterText_->ChangeValue(wxui::toWx(filterTerm_));
        }
        selectedRow_ = 0;
        refreshGrid();
        updateStatus();
    }

    void onFilterText(wxCommandEvent&) {
        filterTerm_ = filterText_ ? wxui::toStd(filterText_->GetValue()) : std::string();
        selectedRow_ = 0;
        refreshGrid();
        updateStatus();
    }

    void onFilterPrompt(wxCommandEvent&) {
        try {
            const auto term = wxui::promptText(this, "Filter/Search", "Search term:", filterTerm_);
            if (term) setFilterTerm(*term);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onClearFilter(wxCommandEvent&) {
        setFilterTerm({});
    }


    void onGeneratePatcherOutput(bool package) {
        try {
            const auto originalPath = wxui::chooseOpenFile(this, "Choose original/base 2DA/GDA", k2DAWildcard);
            if (!originalPath) return;
            TwoDAFile original(*originalPath);
            const std::string patchFilename = originalPath->filename().string().empty() ? std::string("table.2da") : originalPath->filename().string();
            auto project = neotsl::diffTwoDA(original.toTable(), table_.toTable(), patchFilename, package, *originalPath);
            neotsl::throwIfUnsupported(project);
            if (package) {
                wxDirDialog dialog(this, "Choose tslpatchdata output folder", wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
                if (dialog.ShowModal() != wxID_OK) return;
                neotsl::writePackage(project, std::filesystem::path(wxui::toStd(dialog.GetPath())), true);
                wxui::showMessage(this, "Patcher Package Generated", "Wrote changes.ini and staged files to the selected folder.");
            } else {
                const auto output = wxui::chooseSaveFile(this, "Save changes.ini fragment", "INI files (*.ini)|*.ini|All files (*.*)|*.*", "changes_fragment.ini");
                if (!output) return;
                neotsl::writeFragment(project, *output);
                wxui::showMessage(this, "Patcher Fragment Generated", "Wrote the TSLPatcher/HoloPatcher INI fragment.");
            }
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onImport(neotabular::Format format) {
        try {
            if (!confirmDiscardIfNeeded()) return;
            const auto chosen = wxui::chooseOpenFile(this, "Import " + neotabular::formatName(format), wildcardForFlatFormat(format));
            if (!chosen) return;
            const neotabular::Table imported = neotabular::readTable(*chosen, format);
            table_ = TwoDAFile::fromTable(imported);
            table_.setFilename({});
            table_.setDirty(true);
            setFilterTerm({});
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onExport(neotabular::Format format) {
        try {
            const auto chosen = wxui::chooseSaveFile(this, "Export " + neotabular::formatName(format), wildcardForFlatFormat(format),
                                                   exportDefaultFilename(table_.filename(), format, "table"));
            if (!chosen) return;
            auto out = table_.toTable();
            if (!filterTerm_.empty()) out = neotabular::filterRows(out, filterTerm_);
            neotabular::writeTable(out, *chosen, format);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onCopyCells(wxCommandEvent&) {
        if (grid_ == nullptr || !wxTheClipboard->Open()) return;
        int top = grid_->GetGridCursorRow();
        int left = grid_->GetGridCursorCol();
        int bottom = top;
        int right = left;
        const wxGridCellCoordsArray blockTop = grid_->GetSelectionBlockTopLeft();
        const wxGridCellCoordsArray blockBottom = grid_->GetSelectionBlockBottomRight();
        if (!blockTop.IsEmpty() && !blockBottom.IsEmpty()) {
            top = blockTop[0].GetRow();
            left = blockTop[0].GetCol();
            bottom = blockBottom[0].GetRow();
            right = blockBottom[0].GetCol();
        }
        if (top < 0 || left < 0 || bottom < top || right < left) {
            wxTheClipboard->Close();
            return;
        }
        neotabular::Table copied;
        for (int r = top; r <= bottom; ++r) {
            std::vector<std::string> row;
            for (int c = left; c <= right; ++c) {
                row.push_back(wxui::toStd(grid_->GetCellValue(r, c)));
            }
            copied.rows.push_back(std::move(row));
        }
        wxTheClipboard->SetData(new wxTextDataObject(wxui::toWx(neotabular::serializeDelimited(copied, '\t'))));
        wxTheClipboard->Close();
    }

    void onPasteCells(wxCommandEvent&) {
        try {
            if (grid_ == nullptr || !wxTheClipboard->Open()) return;
            if (!wxTheClipboard->IsSupported(wxDF_TEXT)) {
                wxTheClipboard->Close();
                return;
            }
            wxTextDataObject data;
            wxTheClipboard->GetData(data);
            wxTheClipboard->Close();
            const auto pasted = neotabular::parseDelimited(wxui::toStd(data.GetText()), '\t');
            const int startRow = grid_->GetGridCursorRow();
            const int startCol = grid_->GetGridCursorCol();
            for (std::size_t r = 0; r < pasted.rows.size(); ++r) {
                const int gridRow = startRow + static_cast<int>(r);
                if (gridRow < 0 || static_cast<std::size_t>(gridRow) >= visibleRows_.size()) continue;
                const std::size_t actualRow = visibleRows_[static_cast<std::size_t>(gridRow)];
                for (std::size_t c = 0; c < pasted.rows[r].size(); ++c) {
                    const int col = startCol + static_cast<int>(c);
                    if (col < 0 || col >= static_cast<int>(table_.columnCount())) continue;
                    table_.setCell(actualRow, static_cast<std::size_t>(col), pasted.rows[r][c]);
                }
            }
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
            refreshGrid();
        }
    }

    void onNew(wxCommandEvent&) {
        try {
            if (!confirmDiscardIfNeeded()) {
                return;
            }
            newTable(true);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onOpen(wxCommandEvent&) {
        try {
            if (!confirmDiscardIfNeeded()) {
                return;
            }
            const auto chosen = wxui::chooseOpenFile(this, "Open 2DA/GDA", k2DAWildcard);
            if (!chosen) {
                return;
            }
            table_.load(*chosen);
            setFilterTerm({});
            selectedRow_ = 0;
            selectedColumn_ = 0;
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onSave(wxCommandEvent&) {
        try {
            if (table_.filename().empty()) {
                saveAs();
                return;
            }
            saveTo(table_.filename());
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onSaveAs(wxCommandEvent&) {
        try {
            saveAs();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onAddRow(wxCommandEvent&) {
        try {
            const std::string defaultLabel = std::to_string(table_.rowCount());
            const auto label = wxui::promptText(this, "Add Row", "Row label:", defaultLabel);
            if (!label) {
                return;
            }
            table_.addRow(*label);
            selectedRow_ = static_cast<int>(table_.rowCount()) - 1;
            selectedColumn_ = 0;
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onAddColumn(wxCommandEvent&) {
        try {
            const std::string defaultLabel = "Column" + std::to_string(table_.columnCount() + 1);
            const auto label = wxui::promptText(this, "Add Column", "Column label:", defaultLabel);
            if (!label) {
                return;
            }
            table_.addColumn(*label);
            selectedColumn_ = static_cast<int>(table_.columnCount()) - 1;
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onCloneRow(wxCommandEvent&) {
        try {
            const int row = selectedRowOrCursor();
            if (row < 0 || row >= static_cast<int>(table_.rowCount())) {
                throw TwoDAError("Select a row to clone.");
            }
            const std::string defaultLabel = std::to_string(table_.rowCount());
            const auto label = wxui::promptText(this, "Clone Row", "New row label:", defaultLabel);
            if (!label) {
                return;
            }
            table_.cloneRow(static_cast<std::size_t>(row), *label);
            selectedRow_ = static_cast<int>(table_.rowCount()) - 1;
            selectedColumn_ = selectedColumnOrCursor();
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onDeleteRow(wxCommandEvent&) {
        try {
            const int gridRow = selectedRowOrCursor();
            if (gridRow < 0 || static_cast<std::size_t>(gridRow) >= visibleRows_.size()) {
                throw TwoDAError("Select a row to delete.");
            }
            const std::size_t actualRow = actualRowForGrid(gridRow);
            if (!wxui::confirm(this, "Delete Row", "Delete selected row " + table_.rowLabel(actualRow) + "?")) {
                return;
            }
            table_.removeRow(actualRow);
            selectedRow_ = std::min(gridRow, static_cast<int>(table_.rowCount()) - 1);
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onDeleteColumn(wxCommandEvent&) {
        try {
            const int col = selectedColumnOrCursor();
            if (col < 0 || col >= static_cast<int>(table_.columnCount())) {
                throw TwoDAError("Select a column to delete.");
            }
            if (!wxui::confirm(this, "Delete Column", "Delete selected column " + table_.columnLabel(static_cast<std::size_t>(col)) + "?")) {
                return;
            }
            table_.removeColumn(static_cast<std::size_t>(col));
            selectedColumn_ = std::min(col, static_cast<int>(table_.columnCount()) - 1);
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onRenameRow(wxCommandEvent&) {
        try {
            const int row = selectedRowOrCursor();
            if (row < 0 || row >= static_cast<int>(table_.rowCount())) {
                throw TwoDAError("Select a row to rename.");
            }
            const auto label = wxui::promptText(this, "Rename Row", "Row label:", table_.rowLabel(static_cast<std::size_t>(row)));
            if (!label) {
                return;
            }
            table_.setRowLabel(static_cast<std::size_t>(row), *label);
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onRenameColumn(wxCommandEvent&) {
        try {
            const int col = selectedColumnOrCursor();
            if (col < 0 || col >= static_cast<int>(table_.columnCount())) {
                throw TwoDAError("Select a column to rename.");
            }
            const auto label = wxui::promptText(this, "Rename Column", "Column label:", table_.columnLabel(static_cast<std::size_t>(col)));
            if (!label) {
                return;
            }
            table_.setColumnLabel(static_cast<std::size_t>(col), *label);
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onCellChanged(wxGridEvent& event) {
        try {
            const int row = event.GetRow();
            const int col = event.GetCol();
            if (row >= 0 && col >= 0) {
                const std::size_t actualRow = actualRowForGrid(row);
                table_.setCell(actualRow,
                               static_cast<std::size_t>(col),
                               wxui::toStd(grid_->GetCellValue(row, col)));
                selectedRow_ = row;
                selectedColumn_ = col;
                updateStatus();
            }
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
            refreshGrid();
        }
        event.Skip();
    }

    void onLabelDoubleClicked(wxGridEvent& event) {
        if (event.GetRow() >= 0) {
            wxCommandEvent dummy(wxEVT_MENU, ID_RenameRow);
            onRenameRow(dummy);
            return;
        }
        if (event.GetCol() >= 0) {
            wxCommandEvent dummy(wxEVT_MENU, ID_RenameColumn);
            onRenameColumn(dummy);
            return;
        }
        event.Skip();
    }

    void onCellSelected(wxGridEvent& event) {
        selectedRow_ = event.GetRow();
        selectedColumn_ = event.GetCol();
        event.Skip();
    }

    void onToggleDarkMode(wxCommandEvent& event) {
        darkMode_ = event.IsChecked();
        wxui::writeDarkMode("Neo2DA", darkMode_);
        applyDarkMode();
    }

    void onClose(wxCloseEvent& event) {
        if (event.CanVeto() && !confirmDiscardIfNeeded()) {
            event.Veto();
            return;
        }
        event.Skip();
    }

    wxMenuItem* darkModeItem_ = nullptr;
    wxTextCtrl* filePath_ = nullptr;
    wxTextCtrl* filterText_ = nullptr;
    wxGrid* grid_ = nullptr;
    TwoDAFile table_;
    std::string filterTerm_;
    std::vector<std::size_t> visibleRows_;
    int selectedRow_ = 0;
    int selectedColumn_ = 0;
    bool darkMode_ = false;
};

class Neo2DAApp final : public wxApp {
public:
    bool OnInit() override {
#if wxCHECK_VERSION(3, 3, 0)
        SetAppearance(Appearance::System);
#endif
        auto* frame = new Neo2DAFrame;
        frame->Show(true);
        if (argc > 1) {
            frame->CallAfter([frame, arg = wxString(argv[1])]() {
                frame->openStartupFile(std::filesystem::path(wxui::toStd(arg)));
            });
        }
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(Neo2DAApp);
