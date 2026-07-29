#include "core/TwoDAFile.hpp"
#include "core/Version.hpp"
#include "wx_ui.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoDocumentTabs.hpp"
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "neo2da_icon.xpm"
#include "TabularData.hpp"
#include "TslPatcher.hpp"

#include <wx/aui/auibook.h>
#include <wx/clipbrd.h>
#include <wx/dir.h>
#include <wx/dirdlg.h>
#include <wx/grid.h>
#include <wx/icon.h>
#include <wx/iconbndl.h>
#include <wx/sizer.h>
#include <wx/wx.h>
#include <wx/version.h>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

using namespace neo2da;

constexpr const char* kAppName = "Neo2DA";
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
    default: throw std::runtime_error("Neo2DA only supports CSV and TSV table import/export.");
    }
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
    ID_CloseTab,
    ID_CloseOtherTabs,
    ID_NextTab,
    ID_PreviousTab,
    ID_DocumentTabs,
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
    ID_FilterColumn,
    ID_ClearColumnFilter,
    ID_ClearAllFilters,
    ID_MoveColumnLeft,
    ID_MoveColumnRight,
    ID_ResetColumnOrder,
    ID_ResetRowOrder,
    ID_ImportCsv,
    ID_ImportTsv,
    ID_ExportCsv,
    ID_ExportTsv,
    ID_GeneratePatcherPackage,
    ID_GeneratePatcherFragment,
    ID_DarkMode,
    ID_FontIncrease,
    ID_FontDecrease,
    ID_FontReset,
    ID_Grid
};

constexpr int kRecentFileBaseId = wxID_HIGHEST + 1000;
constexpr int kClearRecentFilesId = kRecentFileBaseId + neosettings::kMaxRecentFiles;

class Neo2DAFrame final : public wxFrame {
public:
    Neo2DAFrame()
        : wxFrame(nullptr, wxID_ANY, wxui::toWx(std::string("Neo2DA v") + kNeo2DAVersion + " (2DA table editor)"), wxDefaultPosition, wxDefaultSize) {
        setApplicationIcon();
        buildMenus();
        buildMainWindow();
        wxui::createStatusBar(*this, 2);
        darkMode_ = wxui::readDarkMode(kAppName);
        fontScale_ = settings_.fontScale();
        fontScaleWheelFilter_.attach(this, [this](int steps) { changeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { applyFontScale(); });
        applyDarkMode();
        wxui::configureResponsiveWindow(*this, wxSize(1050, 720), wxSize(620, 420));
        settings_.restoreWindowPlacement(*this);
        createDocumentTab(false);
    }

    void openStartupFile(const std::filesystem::path& path) {
        if (path.empty()) {
            return;
        }
        try {
            openTablePath(path, false);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

private:

    struct DocumentTab {
        std::unique_ptr<TwoDAFile> table = std::make_unique<TwoDAFile>();
        neoview::DocumentViewState viewState;
        std::string untitledName = "Untitled 2DA";
        wxWindow* tabPage = nullptr;
    };

    bool hasActiveDocument() const {
        return activeDocumentIndex_ != neotabs::npos && activeDocumentIndex_ < documents_.size();
    }

    DocumentTab& activeDocument() { return documents_.at(activeDocumentIndex_); }
    const DocumentTab& activeDocument() const { return documents_.at(activeDocumentIndex_); }
    TwoDAFile& table() { return *activeDocument().table; }
    const TwoDAFile& table() const { return *activeDocument().table; }
    neoview::DocumentViewState& viewState() { return activeDocument().viewState; }
    const neoview::DocumentViewState& viewState() const { return activeDocument().viewState; }

    std::string tabDisplayName(const DocumentTab& tab) const {
        return neotabs::displayNameForPath(tab.table ? tab.table->filename() : std::filesystem::path{}, tab.untitledName);
    }

    bool tabDirty(const DocumentTab& tab) const { return tab.table && tab.table->dirty(); }

    void updateActiveTabTitle() {
        if (!hasActiveDocument()) return;
        neotabs::setTabLabel(documentTabs_, activeDocument().tabPage, tabDisplayName(activeDocument()), tabDirty(activeDocument()));
    }

    void selectDocumentTab(std::size_t index) {
        if (documentTabs_ == nullptr || index >= documents_.size()) return;
        tabSwitchInProgress_ = true;
        const bool selected = neotabs::changeSelectionToPage(documentTabs_, documents_[index].tabPage);
        tabSwitchInProgress_ = false;
        if (!selected) return;
        activeDocumentIndex_ = index;
        refreshGrid();
        updateStatus();
    }

    void createDocumentTab(bool markDirty, bool select = true) {
        DocumentTab tab;
        tab.viewState.resetForNewDocument();
        tab.table = std::make_unique<TwoDAFile>(TwoDAFile::create({"Column1"}, 1));
        tab.table->setFilename({});
        tab.table->setDirty(markDirty);
        tab.viewState.selectedVisualRow = 0;
        tab.viewState.selectedVisualColumn = 0;
        const std::size_t previousActiveIndex = activeDocumentIndex_;
        documents_.push_back(std::move(tab));
        const std::size_t index = documents_.size() - 1;

        tabSwitchInProgress_ = true;
        wxWindow* const page = neotabs::addTabPage(
            documentTabs_, tabDisplayName(documents_.back()), tabDirty(documents_.back()), select);
        if (page != nullptr) documents_.back().tabPage = page;
        tabSwitchInProgress_ = false;

        if (page == nullptr) {
            documents_.pop_back();
            activeDocumentIndex_ = previousActiveIndex;
            throw std::runtime_error("Unable to create a document tab.");
        }

        if (select) {
            activeDocumentIndex_ = index;
            tabSwitchInProgress_ = true;
            neotabs::changeSelectionToPage(documentTabs_, page);
            tabSwitchInProgress_ = false;
            refreshGrid();
            updateStatus();
        }
    }

    bool activeTabIsReusableForOpen() const {
        return hasActiveDocument() && documents_.size() == 1 && !tabDirty(activeDocument()) && table().filename().empty() && table().rowCount() == 1 && table().columnCount() == 1;
    }

    void ensureDocumentTabForOpen() {
        if (!hasActiveDocument()) {
            createDocumentTab(false);
            return;
        }
        if (!activeTabIsReusableForOpen()) createDocumentTab(false);
    }

    bool confirmCloseDocumentTab(std::size_t index) {
        if (index >= documents_.size()) return true;
        if (!tabDirty(documents_[index])) return true;
        return wxui::confirm(this, "Close tab", neotabs::closePromptText(tabDisplayName(documents_[index])));
    }

    bool closeDocumentTab(std::size_t index) {
        if (index >= documents_.size() || !confirmCloseDocumentTab(index)) return false;

        wxWindow* const page = documents_[index].tabPage;
        tabSwitchInProgress_ = true;
        const bool deleted = neotabs::deleteTabPage(documentTabs_, page);
        tabSwitchInProgress_ = false;
        if (!deleted) return false;

        documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));
        if (documents_.empty()) {
            activeDocumentIndex_ = neotabs::npos;
            createDocumentTab(false);
            return true;
        }

        std::size_t selectedIndex = neotabs::findDocumentIndexForPage(
            documents_, neotabs::currentPage(documentTabs_));
        if (selectedIndex == neotabs::npos) selectedIndex = std::min(index, documents_.size() - 1);
        selectDocumentTab(selectedIndex);
        return true;
    }

    bool confirmCloseAllTabs() {
        for (std::size_t i = 0; i < documents_.size(); ++i) {
            if (!confirmCloseDocumentTab(i)) return false;
        }
        return true;
    }

    void onDocumentTabChanged(wxAuiNotebookEvent& event) {
        if (tabSwitchInProgress_) { event.Skip(); return; }
        const int selection = event.GetSelection();
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::pageForIndex(documentTabs_, selection));
        if (index != neotabs::npos) selectDocumentTab(index);
        event.Skip();
    }

    void onDocumentTabCloseRequested(wxAuiNotebookEvent& event) {
        event.Veto();
        const int selection = event.GetSelection();
        if (selection < 0) return;
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::pageForIndex(documentTabs_, selection));
        if (index != neotabs::npos) closeDocumentTab(index);
    }

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

    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;

    void buildMenus() {
        auto* file = new wxMenu;
        file->Append(ID_New, "&New 2DA");
        file->Append(ID_Open, "&Open 2DA/GDA...");
        recentFilesMenu_ = new wxMenu;
        rebuildRecentFilesMenu();
        file->AppendSubMenu(recentFilesMenu_, "Open &Recent");
        file->Append(ID_Save, "&Save");
        file->Append(ID_SaveAs, "Save &As...");
        file->AppendSeparator();
        file->Append(ID_CloseTab, "&Close Tab\tCtrl-W");
        file->Append(ID_CloseOtherTabs, "Close &Other Tabs");
        file->Append(ID_NextTab, "Next Tab\tCtrl-Tab");
        file->Append(ID_PreviousTab, "Previous Tab\tCtrl-Shift-Tab");
        gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
            *this, *file, [this](const std::filesystem::path& directory) {
                chooseAndOpen(directory);
            });
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
        edit->Append(ID_FilterColumn, "Filter Selected &Column...");
        edit->Append(ID_ClearColumnFilter, "Clear Filter on Selected Column");
        edit->Append(ID_ClearAllFilters, "Clear &All Filters");
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
        view->AppendSeparator();
        view->Append(ID_FontIncrease, "Increase Font Size\tCtrl++");
        view->Append(ID_FontDecrease, "Decrease Font Size\tCtrl+-");
        view->Append(ID_FontReset, "Reset Font Size\tCtrl+0");

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
        Bind(wxEVT_MENU, &Neo2DAFrame::onOpenRecent, this, kRecentFileBaseId, kRecentFileBaseId + neosettings::kMaxRecentFiles - 1);
        Bind(wxEVT_MENU, &Neo2DAFrame::onClearRecentFiles, this, kClearRecentFilesId);
        Bind(wxEVT_MENU, &Neo2DAFrame::onSave, this, ID_Save);
        Bind(wxEVT_MENU, &Neo2DAFrame::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_MENU, &Neo2DAFrame::onCloseTab, this, ID_CloseTab);
        Bind(wxEVT_MENU, &Neo2DAFrame::onCloseOtherTabs, this, ID_CloseOtherTabs);
        Bind(wxEVT_MENU, &Neo2DAFrame::onNextTab, this, ID_NextTab);
        Bind(wxEVT_MENU, &Neo2DAFrame::onPreviousTab, this, ID_PreviousTab);
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
        Bind(wxEVT_MENU, &Neo2DAFrame::onFilterSelectedColumn, this, ID_FilterColumn);
        Bind(wxEVT_MENU, &Neo2DAFrame::onClearSelectedColumnFilter, this, ID_ClearColumnFilter);
        Bind(wxEVT_MENU, &Neo2DAFrame::onClearAllFilters, this, ID_ClearAllFilters);
        Bind(wxEVT_MENU, &Neo2DAFrame::onMoveColumnLeft, this, ID_MoveColumnLeft);
        Bind(wxEVT_MENU, &Neo2DAFrame::onMoveColumnRight, this, ID_MoveColumnRight);
        Bind(wxEVT_MENU, &Neo2DAFrame::onResetColumnOrder, this, ID_ResetColumnOrder);
        Bind(wxEVT_MENU, &Neo2DAFrame::onResetRowOrder, this, ID_ResetRowOrder);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(neotabular::Format::Csv); }, ID_ImportCsv);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(neotabular::Format::Tsv); }, ID_ImportTsv);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(neotabular::Format::Csv); }, ID_ExportCsv);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(neotabular::Format::Tsv); }, ID_ExportTsv);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onGeneratePatcherOutput(true); }, ID_GeneratePatcherPackage);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onGeneratePatcherOutput(false); }, ID_GeneratePatcherFragment);
        Bind(wxEVT_MENU, &Neo2DAFrame::onToggleDarkMode, this, ID_DarkMode);
        Bind(wxEVT_MENU, &Neo2DAFrame::onIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &Neo2DAFrame::onDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &Neo2DAFrame::onResetFontScale, this, ID_FontReset);
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

        documentTabs_ = new wxAuiNotebook(panel, ID_DocumentTabs, wxDefaultPosition, wxDefaultSize,
                                          wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ACTIVE_TAB | wxAUI_NB_SCROLL_BUTTONS);
        root->Add(documentTabs_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
        neotabs::configureDocumentTabStrip(documentTabs_);

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
        wxui::configureStableGridRendering(*grid_);
        grid_->SetRowLabelSize(95);
        grid_->EnableEditing(true);
        grid_->EnableDragColMove(true);
        grid_->SetSelectionMode(wxGrid::wxGridSelectCells);
        root->Add(grid_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        auto* buttons = new wxWrapSizer(wxHORIZONTAL);
        const auto addCommandButton = [&](int id, const wxString& label) {
            buttons->Add(new wxButton(panel, id, label), 0,
                         wxRIGHT | wxBOTTOM, FromDIP(6));
        };
        addCommandButton(ID_AddRow, "Add Row...");
        addCommandButton(ID_AddColumn, "Add Column...");
        addCommandButton(ID_CloneRow, "Clone Row...");
        addCommandButton(ID_DeleteRow, "Delete Row");
        addCommandButton(ID_DeleteColumn, "Delete Column");
        addCommandButton(ID_RenameRow, "Rename Row...");
        addCommandButton(ID_RenameColumn, "Rename Column...");
        root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(2));

        panel->SetSizer(root);

        filterText_->Bind(wxEVT_TEXT, &Neo2DAFrame::onFilterText, this);
        grid_->Bind(wxEVT_GRID_CELL_CHANGED, &Neo2DAFrame::onCellChanged, this);
        grid_->Bind(wxEVT_GRID_LABEL_LEFT_DCLICK, &Neo2DAFrame::onLabelDoubleClicked, this);
        grid_->Bind(wxEVT_GRID_LABEL_RIGHT_CLICK, &Neo2DAFrame::onGridLabelRightClick, this);
        grid_->Bind(wxEVT_GRID_COL_MOVE, &Neo2DAFrame::onGridColumnMoved, this);
        grid_->Bind(wxEVT_GRID_SELECT_CELL, &Neo2DAFrame::onCellSelected, this);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &Neo2DAFrame::onDocumentTabChanged, this);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &Neo2DAFrame::onDocumentTabCloseRequested, this);
    }

    void newTable(bool markDirty) {
        createDocumentTab(markDirty);
    }

    bool confirmDiscardIfNeeded() {
        if (!table().dirty()) {
            return true;
        }
        return wxui::confirm(this, "Discard changes", "The open 2DA has unsaved changes. Continue without saving?");
    }

    bool openTablePath(const std::filesystem::path& path, bool checkDirty = true) {
        if (path.empty()) return false;
        if (checkDirty) ensureDocumentTabForOpen();
        table().load(path);
        viewState().resetForNewDocument();
        setFilterTerm({});
        viewState().selectedVisualRow = 0;
        viewState().selectedVisualColumn = 0;
        refreshGrid();
        updateStatus();
        rememberRecentFile(path);
        neogames::resolver().inferFromOpenedPath(path);
        return true;
    }

    void rebuildRecentFilesMenu() {
        if (recentFilesMenu_ != nullptr) {
            neosettings::populateRecentFilesMenu(*recentFilesMenu_, settings_, kRecentFileBaseId, kClearRecentFilesId);
        }
    }

    void rememberRecentFile(const std::filesystem::path& path) {
        settings_.addRecentFile(path);
        rebuildRecentFilesMenu();
    }

    void onOpenRecent(wxCommandEvent& event) {
        const int index = event.GetId() - kRecentFileBaseId;
        const auto files = settings_.recentFiles();
        if (index < 0 || static_cast<std::size_t>(index) >= files.size()) return;
        try {
            if (!std::filesystem::exists(files[static_cast<std::size_t>(index)])) {
                settings_.removeRecentFile(files[static_cast<std::size_t>(index)]);
                rebuildRecentFilesMenu();
                throw TwoDAError("Recent file no longer exists: " + files[static_cast<std::size_t>(index)].string());
            }
            openTablePath(files[static_cast<std::size_t>(index)], true);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onClearRecentFiles(wxCommandEvent&) {
        settings_.clearRecentFiles();
        rebuildRecentFilesMenu();
    }

    std::size_t actualRowForGrid(int gridRow) const {
        try {
            return neoview::logicalRowForVisual(viewState(), gridRow);
        } catch (const std::out_of_range&) {
            throw TwoDAError("Selected row is outside the current filtered view.");
        }
    }

    std::size_t actualColumnForGrid(int gridColumn) const {
        try {
            return neoview::logicalColumnForVisual(viewState(), gridColumn);
        } catch (const std::out_of_range&) {
            throw TwoDAError("Selected column is outside the current view.");
        }
    }

    int visibleRowForActual(std::size_t actualRow) const {
        return neoview::visualRowForLogical(viewState(), actualRow);
    }

    void resetNativeGridColumnOrder() {
        if (grid_ == nullptr) return;
        nativeColumnOrderSyncInProgress_ = true;
        grid_->ResetColPos();
        nativeColumnOrderSyncInProgress_ = false;
    }

    void syncDraggedColumnOrder() {
        columnDragSyncPending_ = false;
        const int draggedVisualColumn = pendingDraggedVisualColumn_;
        pendingDraggedVisualColumn_ = -1;
        if (!hasActiveDocument() || grid_ == nullptr || !table().loaded()) return;

        const int columnCount = grid_->GetNumberCols();
        if (columnCount <= 0) {
            resetNativeGridColumnOrder();
            return;
        }

        neoview::ensureIdentityColumns(viewState(), table().columnCount());
        const auto previousOrder = viewState().visualToLogicalColumns;
        if (previousOrder.size() != static_cast<std::size_t>(columnCount)) {
            resetNativeGridColumnOrder();
            refreshGrid();
            updateStatus();
            return;
        }

        std::vector<std::size_t> nextOrder;
        nextOrder.reserve(previousOrder.size());
        std::vector<bool> seen(previousOrder.size(), false);
        bool changed = false;
        for (int visualPosition = 0; visualPosition < columnCount; ++visualPosition) {
            const int previousVisualColumn = grid_->GetColAt(visualPosition);
            if (previousVisualColumn < 0 || previousVisualColumn >= columnCount || seen[static_cast<std::size_t>(previousVisualColumn)]) {
                resetNativeGridColumnOrder();
                refreshGrid();
                updateStatus();
                return;
            }
            nextOrder.push_back(previousOrder[static_cast<std::size_t>(previousVisualColumn)]);
            seen[static_cast<std::size_t>(previousVisualColumn)] = true;
            if (previousVisualColumn != visualPosition) changed = true;
        }

        resetNativeGridColumnOrder();
        if (!changed || nextOrder == previousOrder) return;

        std::size_t selectedLogicalColumn = 0;
        if (draggedVisualColumn >= 0 &&
            static_cast<std::size_t>(draggedVisualColumn) < previousOrder.size()) {
            selectedLogicalColumn = previousOrder[static_cast<std::size_t>(draggedVisualColumn)];
        } else if (viewState().selectedVisualColumn >= 0 &&
                   static_cast<std::size_t>(viewState().selectedVisualColumn) < previousOrder.size()) {
            selectedLogicalColumn = previousOrder[static_cast<std::size_t>(viewState().selectedVisualColumn)];
        } else if (viewState().selectedLogicalColumn >= 0 &&
                   static_cast<std::size_t>(viewState().selectedLogicalColumn) < table().columnCount()) {
            selectedLogicalColumn = static_cast<std::size_t>(viewState().selectedLogicalColumn);
        }

        viewState().visualToLogicalColumns = std::move(nextOrder);
        viewState().selectedLogicalColumn = static_cast<int>(selectedLogicalColumn);
        const int newVisualColumn = neoview::visualColumnForLogical(viewState(), selectedLogicalColumn);
        if (newVisualColumn >= 0) viewState().selectedVisualColumn = newVisualColumn;

        refreshGrid();
        updateStatus();
    }

    bool rowPassesCurrentFilters(const neotabular::Table& allRows, std::size_t row) const {
        if (row >= allRows.rows.size()) return false;
        if (!viewState().filterTerm.empty() && !neotabular::rowMatches(allRows, allRows.rows[row], viewState().filterTerm)) {
            return false;
        }
        return neoview::rowPassesColumnFilters(viewState(), [&](std::size_t logicalColumn) {
            return logicalColumn < table().columnCount() ? table().cell(row, logicalColumn) : std::string();
        });
    }

    neotabular::Table filteredExportTable() const {
        neotabular::Table out = table().toTable();
        if (!neoview::hasAnyFilter(viewState())) return out;
        std::vector<std::vector<std::string>> rows;
        rows.reserve(out.rows.size());
        for (std::size_t row = 0; row < table().rowCount(); ++row) {
            if (rowPassesCurrentFilters(out, row) && row < out.rows.size()) {
                rows.push_back(out.rows[row]);
            }
        }
        out.rows = std::move(rows);
        return out;
    }

    void rebuildVisibleRows() {
        viewState().visualToLogicalRows.clear();
        if (!table().loaded()) return;
        neoview::removeColumnFiltersOutsideRange(viewState(), table().columnCount());
        auto allRows = table().toTable();
        std::vector<std::size_t> visibleRows;
        visibleRows.reserve(table().rowCount());
        for (std::size_t row = 0; row < table().rowCount(); ++row) {
            if (rowPassesCurrentFilters(allRows, row)) {
                visibleRows.push_back(row);
            }
        }
        neoview::setRowsFromLogicalRows(viewState(), std::move(visibleRows));
    }

    void refreshGrid() {
        if (!table().loaded()) {
            return;
        }
        rebuildVisibleRows();
        neoview::ensureIdentityColumns(viewState(), table().columnCount());

        const int wantedRows = static_cast<int>(viewState().visualToLogicalRows.size());
        const int wantedCols = static_cast<int>(viewState().visualToLogicalColumns.size());
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
            const std::size_t actualColumn = actualColumnForGrid(c);
            std::string columnLabel = table().columnLabel(actualColumn);
            if (neoview::findColumnFilter(viewState(), actualColumn) != nullptr) columnLabel += " *";
            grid_->SetColLabelValue(c, wxui::toWx(columnLabel));
            if (grid_->GetColSize(c) < FromDIP(90)) {
                grid_->SetColSize(c, FromDIP(120));
            }
        }
        for (int r = 0; r < wantedRows; ++r) {
            const std::size_t actualRow = viewState().visualToLogicalRows[static_cast<std::size_t>(r)];
            grid_->SetRowLabelValue(r, wxui::toWx(table().rowLabel(actualRow)));
            for (int c = 0; c < wantedCols; ++c) {
                const std::size_t actualColumn = actualColumnForGrid(c);
                grid_->SetCellValue(r, c, wxui::toWx(table().cell(actualRow, actualColumn)));
                grid_->SetReadOnly(r, c, false);
            }
        }
        wxui::applyGridTheme(*grid_, darkMode_);
        if (viewState().selectedVisualRow >= wantedRows) viewState().selectedVisualRow = wantedRows - 1;
        if (viewState().selectedVisualColumn >= wantedCols) viewState().selectedVisualColumn = wantedCols - 1;
        if (viewState().selectedVisualRow >= 0 && viewState().selectedVisualRow < wantedRows && viewState().selectedVisualColumn >= 0 && viewState().selectedVisualColumn < wantedCols) {
            grid_->SetGridCursor(viewState().selectedVisualRow, viewState().selectedVisualColumn);
            grid_->MakeCellVisible(viewState().selectedVisualRow, viewState().selectedVisualColumn);
        }
    }

    void updateStatus() {
        updateActiveTabTitle();
        filePath_->SetValue(wxui::toWx(pathText(table().filename())));
        const std::string name = table().filename().empty() ? "Untitled 2DA" : table().filename().filename().string();
        wxui::setStatusText(*this, wxui::toWx(name + (table().dirty() ? " modified" : "")), 0);
        if (table().loaded()) {
            std::string detail = std::to_string(viewState().visualToLogicalRows.size()) + "/" +
                                 std::to_string(table().rowCount()) + " rows, " +
                                 std::to_string(table().columnCount()) + " columns";
            const std::string columnFilters = neoview::columnFilterSummary(viewState());
            if (!columnFilters.empty()) detail += "; filters: " + columnFilters;
            wxui::setStatusText(*this, wxui::toWx(detail), 1);
        } else {
            wxui::setStatusText(*this, "No table", 1);
        }
    }

    void saveTo(const std::filesystem::path& path) {
        table().save(path);
        table().setFilename(path);
        table().setDirty(false);
        rememberRecentFile(path);
        neogames::resolver().inferFromOpenedPath(path);
        updateStatus();
    }

    bool saveAs() {
        const std::string defaultName = table().filename().empty() ? "new.2da" : table().filename().filename().string();
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
        applyFontScale();
    }

    void applyFontScale() {
        neoview::applyFontScale(this, fontScale_);
    }

    void changeFontScaleSteps(int steps) {
        const double next = neoview::steppedFontScale(fontScale_, steps);
        if (neoview::fontScalePercent(next) == neoview::fontScalePercent(fontScale_)) return;
        fontScale_ = next;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }

    void setFilterTerm(std::string term) {
        viewState().filterTerm = std::move(term);
        if (filterText_ != nullptr && wxui::toStd(filterText_->GetValue()) != viewState().filterTerm) {
            filterText_->ChangeValue(wxui::toWx(viewState().filterTerm));
        }
        viewState().selectedVisualRow = 0;
        refreshGrid();
        updateStatus();
    }

    void onFilterText(wxCommandEvent&) {
        viewState().filterTerm = filterText_ ? wxui::toStd(filterText_->GetValue()) : std::string();
        viewState().selectedVisualRow = 0;
        refreshGrid();
        updateStatus();
    }

    void onFilterPrompt(wxCommandEvent&) {
        try {
            const auto term = wxui::promptText(this, "Filter/Search", "Search term:", viewState().filterTerm);
            if (term) setFilterTerm(*term);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void clearAllFiltersAndRefresh() {
        neoview::clearAllFilters(viewState());
        if (filterText_ != nullptr && !filterText_->GetValue().empty()) {
            filterText_->ChangeValue(wxString{});
        }
        viewState().selectedVisualRow = 0;
        refreshGrid();
        updateStatus();
    }

    int filterContextColumn() const {
        if (contextVisualColumn_ >= 0) return contextVisualColumn_;
        return selectedColumnOrCursor();
    }

    void promptColumnFilterForVisualColumn(int visualColumn) {
        if (visualColumn < 0 || static_cast<std::size_t>(visualColumn) >= viewState().visualToLogicalColumns.size()) {
            throw TwoDAError("Select a column to filter.");
        }
        const std::size_t logicalColumn = actualColumnForGrid(visualColumn);
        const auto* existing = neoview::findColumnFilter(viewState(), logicalColumn);
        const std::string label = table().columnLabel(logicalColumn);
        const auto term = wxui::promptText(this, "Filter Column", "Show rows where column '" + label + "' contains:", existing ? existing->term : std::string());
        if (!term) return;
        neoview::setColumnFilter(viewState(), neoview::ColumnFilter{logicalColumn, label, *term, neoview::TextFilterMode::Contains, true});
        viewState().selectedVisualColumn = visualColumn;
        viewState().selectedVisualRow = 0;
        refreshGrid();
        updateStatus();
    }

    void onClearFilter(wxCommandEvent&) {
        clearAllFiltersAndRefresh();
    }

    void onFilterSelectedColumn(wxCommandEvent&) {
        try {
            promptColumnFilterForVisualColumn(filterContextColumn());
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
        contextVisualColumn_ = -1;
    }

    void onClearSelectedColumnFilter(wxCommandEvent&) {
        try {
            const int visualColumn = filterContextColumn();
            const std::size_t logicalColumn = actualColumnForGrid(visualColumn);
            neoview::clearColumnFilter(viewState(), logicalColumn);
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
        contextVisualColumn_ = -1;
    }

    void onClearAllFilters(wxCommandEvent&) {
        clearAllFiltersAndRefresh();
    }

    void onMoveColumnLeft(wxCommandEvent&) {
        const int visualColumn = selectedColumnOrCursor();
        if (neoview::moveVisualColumn(viewState(), visualColumn, visualColumn - 1)) {
            viewState().selectedVisualColumn = visualColumn - 1;
            refreshGrid();
        }
    }

    void onMoveColumnRight(wxCommandEvent&) {
        const int visualColumn = selectedColumnOrCursor();
        if (neoview::moveVisualColumn(viewState(), visualColumn, visualColumn + 1)) {
            viewState().selectedVisualColumn = visualColumn + 1;
            refreshGrid();
        }
    }

    void onResetColumnOrder(wxCommandEvent&) {
        neoview::setIdentityColumns(viewState(), table().columnCount());
        viewState().selectedVisualColumn = 0;
        refreshGrid();
    }

    void onResetRowOrder(wxCommandEvent&) {
        viewState().sortColumn = 0;
        viewState().sortAscending = true;
        rebuildVisibleRows();
        viewState().selectedVisualRow = 0;
        refreshGrid();
        updateStatus();
    }

    void onGridLabelRightClick(wxGridEvent& event) {
        if (event.GetCol() >= 0) {
            contextVisualColumn_ = event.GetCol();
            wxMenu menu;
            menu.Append(ID_FilterColumn, "Filter This Column...");
            menu.Append(ID_ClearColumnFilter, "Clear Filter on This Column");
            menu.AppendSeparator();
            menu.Append(ID_MoveColumnLeft, "Move Column Left");
            menu.Append(ID_MoveColumnRight, "Move Column Right");
            menu.Append(ID_ResetColumnOrder, "Reset Column Order");
            PopupMenu(&menu);
            return;
        }
        event.Skip();
    }

    void onGeneratePatcherOutput(bool package) {
        try {
            if (table().isGda()) {
                throw std::runtime_error(
                    "TSLPatcher/HoloPatcher [2DAList] supports KotOR-style 2DA files only. "
                    "Dragon Age GDA files can be edited and exported as CSV/TSV, but cannot be emitted as 2DA patch instructions.");
            }
            const auto originalPath = wxui::chooseOpenFile(this, "Choose original/base KotOR 2DA", "KotOR 2DA files (*.2da)|*.2da|All files (*.*)|*.*");
            if (!originalPath) return;
            TwoDAFile original(*originalPath);
            if (original.isGda()) {
                throw std::runtime_error("The selected baseline is a Dragon Age GDA file; choose a KotOR-style 2DA file.");
            }
            const std::string patchFilename = originalPath->filename().string().empty() ? std::string("table.2da") : originalPath->filename().string();
            auto project = neotsl::diffTwoDA(original.toTable(), table().toTable(), patchFilename, package, *originalPath);
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
            const auto chosen = wxui::chooseOpenFile(this, "Import " + neotabular::formatName(format), wildcardForFlatFormat(format));
            if (!chosen) return;
            const neotabular::Table imported = neotabular::readTable(*chosen, format);
            ensureDocumentTabForOpen();
            table() = TwoDAFile::fromTable(imported);
            table().setFilename({});
            table().setDirty(true);
            viewState().resetForNewDocument();
            if (filterText_) filterText_->ChangeValue("");
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onExport(neotabular::Format format) {
        try {
            const auto chosen = wxui::chooseSaveFile(this, "Export " + neotabular::formatName(format), wildcardForFlatFormat(format),
                                                   exportDefaultFilename(table().filename(), format, "table"));
            if (!chosen) return;
            auto out = filteredExportTable();
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
                if (gridRow < 0 || static_cast<std::size_t>(gridRow) >= viewState().visualToLogicalRows.size()) continue;
                const std::size_t actualRow = viewState().visualToLogicalRows[static_cast<std::size_t>(gridRow)];
                for (std::size_t c = 0; c < pasted.rows[r].size(); ++c) {
                    const int col = startCol + static_cast<int>(c);
                    if (col < 0 || col >= static_cast<int>(viewState().visualToLogicalColumns.size())) continue;
                    const std::size_t actualColumn = actualColumnForGrid(col);
                    table().setCell(actualRow, actualColumn, pasted.rows[r][c]);
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
            newTable(true);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void chooseAndOpen(const std::filesystem::path& initialDirectory = {}) {
        try {
            const auto chosen = wxui::chooseOpenFile(this, "Open 2DA/GDA", k2DAWildcard, initialDirectory);
            if (!chosen) {
                return;
            }
            openTablePath(*chosen, true);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onOpen(wxCommandEvent&) {
        chooseAndOpen();
    }

    void onSave(wxCommandEvent&) {
        try {
            if (table().filename().empty()) {
                saveAs();
                return;
            }
            saveTo(table().filename());
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
            const std::string defaultLabel = std::to_string(table().rowCount());
            const auto label = wxui::promptText(this, "Add Row", "Row label:", defaultLabel);
            if (!label) {
                return;
            }
            table().addRow(*label);
            viewState().selectedVisualRow = static_cast<int>(table().rowCount()) - 1;
            viewState().selectedVisualColumn = 0;
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onAddColumn(wxCommandEvent&) {
        try {
            const std::string defaultLabel = "Column" + std::to_string(table().columnCount() + 1);
            const auto label = wxui::promptText(this, "Add Column", "Column label:", defaultLabel);
            if (!label) {
                return;
            }
            table().addColumn(*label);
            viewState().selectedVisualColumn = static_cast<int>(table().columnCount()) - 1;
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onCloneRow(wxCommandEvent&) {
        try {
            const int row = selectedRowOrCursor();
            if (row < 0 || static_cast<std::size_t>(row) >= viewState().visualToLogicalRows.size()) {
                throw TwoDAError("Select a row to clone.");
            }
            const std::size_t actualRow = actualRowForGrid(row);
            const std::string defaultLabel = std::to_string(table().rowCount());
            const auto label = wxui::promptText(this, "Clone Row", "New row label:", defaultLabel);
            if (!label) {
                return;
            }
            table().cloneRow(actualRow, *label);
            viewState().selectedVisualRow = static_cast<int>(table().rowCount()) - 1;
            viewState().selectedVisualColumn = selectedColumnOrCursor();
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onDeleteRow(wxCommandEvent&) {
        try {
            const int gridRow = selectedRowOrCursor();
            if (gridRow < 0 || static_cast<std::size_t>(gridRow) >= viewState().visualToLogicalRows.size()) {
                throw TwoDAError("Select a row to delete.");
            }
            const std::size_t actualRow = actualRowForGrid(gridRow);
            if (!wxui::confirm(this, "Delete Row", "Delete selected row " + table().rowLabel(actualRow) + "?")) {
                return;
            }
            table().removeRow(actualRow);
            viewState().selectedVisualRow = std::min(gridRow, static_cast<int>(table().rowCount()) - 1);
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onDeleteColumn(wxCommandEvent&) {
        try {
            const int col = selectedColumnOrCursor();
            if (col < 0 || static_cast<std::size_t>(col) >= viewState().visualToLogicalColumns.size()) {
                throw TwoDAError("Select a column to delete.");
            }
            const std::size_t actualColumn = actualColumnForGrid(col);
            if (!wxui::confirm(this, "Delete Column", "Delete selected column " + table().columnLabel(actualColumn) + "?")) {
                return;
            }
            table().removeColumn(actualColumn);
            viewState().selectedVisualColumn = std::min(col, static_cast<int>(table().columnCount()) - 1);
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onRenameRow(wxCommandEvent&) {
        try {
            const int row = selectedRowOrCursor();
            if (row < 0 || static_cast<std::size_t>(row) >= viewState().visualToLogicalRows.size()) {
                throw TwoDAError("Select a row to rename.");
            }
            const std::size_t actualRow = actualRowForGrid(row);
            const auto label = wxui::promptText(this, "Rename Row", "Row label:", table().rowLabel(actualRow));
            if (!label) {
                return;
            }
            table().setRowLabel(actualRow, *label);
            refreshGrid();
            updateStatus();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onRenameColumn(wxCommandEvent&) {
        try {
            const int col = selectedColumnOrCursor();
            if (col < 0 || static_cast<std::size_t>(col) >= viewState().visualToLogicalColumns.size()) {
                throw TwoDAError("Select a column to rename.");
            }
            const std::size_t actualColumn = actualColumnForGrid(col);
            const auto label = wxui::promptText(this, "Rename Column", "Column label:", table().columnLabel(actualColumn));
            if (!label) {
                return;
            }
            table().setColumnLabel(actualColumn, *label);
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
                const std::size_t actualColumn = actualColumnForGrid(col);
                table().setCell(actualRow,
                               actualColumn,
                               wxui::toStd(grid_->GetCellValue(row, col)));
                viewState().selectedVisualRow = row;
                viewState().selectedVisualColumn = col;
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

    void onGridColumnMoved(wxGridEvent& event) {
        if (!nativeColumnOrderSyncInProgress_ && !columnDragSyncPending_) {
            pendingDraggedVisualColumn_ = event.GetCol();
            columnDragSyncPending_ = true;
            CallAfter([this]() { syncDraggedColumnOrder(); });
        }
        event.Skip();
    }

    void onCellSelected(wxGridEvent& event) {
        viewState().selectedVisualRow = event.GetRow();
        viewState().selectedVisualColumn = event.GetCol();
        try {
            viewState().selectedLogicalRow = static_cast<int>(actualRowForGrid(event.GetRow()));
            viewState().selectedLogicalColumn = static_cast<int>(actualColumnForGrid(event.GetCol()));
        } catch (...) {
        }
        event.Skip();
    }

    void onCloseTab(wxCommandEvent&) { closeDocumentTab(activeDocumentIndex_); }

    void onCloseOtherTabs(wxCommandEvent&) {
        if (!hasActiveDocument()) return;
        for (std::size_t i = documents_.size(); i-- > 0;) {
            if (i != activeDocumentIndex_ && !closeDocumentTab(i)) return;
        }
    }

    void onNextTab(wxCommandEvent&) {
        if (documentTabs_ == nullptr || documentTabs_->GetPageCount() < 2) return;
        tabSwitchInProgress_ = true;
        documentTabs_->AdvanceSelection(true);
        tabSwitchInProgress_ = false;
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::currentPage(documentTabs_));
        if (index != neotabs::npos) selectDocumentTab(index);
    }

    void onPreviousTab(wxCommandEvent&) {
        if (documentTabs_ == nullptr || documentTabs_->GetPageCount() < 2) return;
        tabSwitchInProgress_ = true;
        documentTabs_->AdvanceSelection(false);
        tabSwitchInProgress_ = false;
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::currentPage(documentTabs_));
        if (index != neotabs::npos) selectDocumentTab(index);
    }

    void onToggleDarkMode(wxCommandEvent& event) {
        darkMode_ = event.IsChecked();
        wxui::writeDarkMode(kAppName, darkMode_);
        applyDarkMode();
    }

    void onIncreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        changeFontScaleSteps(1);
    }
    void onDecreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        changeFontScaleSteps(-1);
    }
    void onResetFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        fontScale_ = neoview::kDefaultFontScale;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }


    void onClose(wxCloseEvent& event) {
        if (event.CanVeto() && !confirmCloseAllTabs()) {
            event.Veto();
            return;
        }
        settings_.saveWindowPlacement(*this);
        event.Skip();
    }

    neosettings::AppSettings settings_{kAppName};
    wxMenu* recentFilesMenu_ = nullptr;
    wxMenuItem* darkModeItem_ = nullptr;
    wxTextCtrl* filePath_ = nullptr;
    wxTextCtrl* filterText_ = nullptr;
    wxGrid* grid_ = nullptr;
    int contextVisualColumn_ = -1;
    int pendingDraggedVisualColumn_ = -1;
    bool columnDragSyncPending_ = false;
    bool nativeColumnOrderSyncInProgress_ = false;
    wxAuiNotebook* documentTabs_ = nullptr;
    std::vector<DocumentTab> documents_;
    std::size_t activeDocumentIndex_ = neotabs::npos;
    bool tabSwitchInProgress_ = false;
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    double fontScale_ = neoview::kDefaultFontScale;
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
