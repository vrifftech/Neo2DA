#include "EditorPanel.hpp"
#include "core/ResourceDocument.hpp"
#include "core/TwoDAFile.hpp"
#include "BrowserOpenSelection.hpp"
#include "core/Version.hpp"
#include "wx_ui.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoDocumentTabs.hpp"
#include "NeoSettings.hpp"
#include "NeoPatcherExport.hpp"
#include "NeoViewState.hpp"
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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

static_assert(wxui::kPatcherExportUiApiVersion >= 3u,
              "Neo2DA requires the exact-INI/Fragment patch-export UI from the current neoshared checkout.");
#if defined(__EMSCRIPTEN__)
static_assert(neobrowser::kBrowserFileApiVersion >= 10u,
              "Neo2DA requires owned browser imports and transactional write-back from the current neoshared checkout.");
#endif

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

#if defined(__EMSCRIPTEN__)
constexpr const char* kBrowserTableAccept = ".2da,.gda,.tsv,.csv";
#endif

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
    ID_New = wxID_HIGHEST + 12000,
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
    ID_GeneratePatcher,
    ID_DarkMode,
    ID_FontIncrease,
    ID_FontDecrease,
    ID_FontReset,
    ID_Grid, ID_ModuleAbout, ID_ModuleExit
};

constexpr int kRecentFileBaseId = wxID_HIGHEST + 14000;
constexpr int kClearRecentFilesId = kRecentFileBaseId + neosettings::kMaxRecentFiles;

class Neo2DAPanelImpl final : public neo2da::ui::EditorPanel {
public:
    Neo2DAPanelImpl(wxWindow* parent, neomodules::Context context = {})
        : EditorPanel(parent, std::move(context)) {
        buildMenus();
        buildMainWindow();
        createModuleStatusBar(2);
        darkMode_ = wxui::readDarkMode(kAppName);
        fontScale_ = settings_.fontScale();
        if (!context_.embedded) fontScaleWheelFilter_.attach(this, [this](int steps) { changeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { applyFontScale(); });
        applyDarkMode();
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

    bool openFile(const std::filesystem::path& path) override { return openTablePath(path,true); }
    bool activateResource(const std::string& identity) override {
        if(identity.empty())return false;
        for(std::size_t i=0;i<documents_.size();++i) if(documents_[i].resourceIdentity==identity) {
            selectDocumentTab(i);return true;
        }
        return false;
    }
    bool openResource(neoshared::ResourceDocument input) override {
        if(activateResource(input.identity))return true;
        auto loaded=neo2da::ResourceDocument::load(std::move(input));
        ensureDocumentTabForOpen();
        auto& document=activeDocument();
        *document.table=std::move(loaded.table);
        document.untitledName=loaded.source.fileName;
        document.resourceIdentity=std::move(loaded.source.identity);
        document.sourceDescription=std::move(loaded.source.sourceDescription);
        document.protectedInputs=std::move(loaded.source.protectedInputs);
        document.viewState.resetForNewDocument();setFilterTerm({});
        refreshGrid();updateStatus();grid_->SetFocus();return true;
    }
    bool saveActiveAs(const std::filesystem::path& path) override { return saveTo(path); }
    std::size_t documentCount() const override {return documents_.size();}
    TwoDAFile* activeTable() override {return hasActiveDocument()?activeDocument().table.get():nullptr;}
    void refreshActiveTable() override {if(hasActiveDocument()){refreshGrid();updateStatus();}}
    std::vector<std::filesystem::path> openPaths() const override {
        std::vector<std::filesystem::path> paths;
        for(const auto& document:documents_) if(!document.table->filename().empty())paths.push_back(document.table->filename());
        return paths;
    }
    bool canClose() override {
        if(browserSaveActive_)return false;
        commitPendingCell();return confirmCloseAllTabs();
    }
    void setAppearance(bool dark,double scale) override {darkMode_=dark;fontScale_=scale;applyDarkMode();}

private:

    struct DocumentTab {
        std::unique_ptr<TwoDAFile> table = std::make_unique<TwoDAFile>();
        neoview::DocumentViewState viewState;
        std::string untitledName = "Untitled 2DA";
        wxWindow* tabPage = nullptr;
        bool saveInProgress = false;
        std::string resourceIdentity;
        std::string sourceDescription;
        std::vector<std::filesystem::path> protectedInputs;
#if defined(__EMSCRIPTEN__)
        neobrowser::BrowserImportLease sourceImport;
#endif
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

    bool tabDirty(const DocumentTab& tab) const {
        return tab.saveInProgress || (tab.table && tab.table->dirty());
    }

    void updateDocumentTabTitle(DocumentTab& document) {
        neotabs::setTabLabel(documentTabs_, document.tabPage,
                             tabDisplayName(document), tabDirty(document));
    }

    void updateActiveTabTitle() {
        if (!hasActiveDocument()) return;
        updateDocumentTabTitle(activeDocument());
    }

    void selectDocumentTab(std::size_t index) {
        if (documentTabs_ == nullptr || index >= documents_.size()) return;
        if(hasActiveDocument() && index!=activeDocumentIndex_)commitPendingCell();
        tabSwitchInProgress_ = true;
        const bool selected = neotabs::changeSelectionToPage(documentTabs_, documents_[index].tabPage);
        tabSwitchInProgress_ = false;
        if (!selected) return;
        activeDocumentIndex_ = index;
        refreshGrid();
        updateStatus();
    }

    void createDocumentTab(bool markDirty, bool select = true) {
        if(select && hasActiveDocument())commitPendingCell();
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
        return hasActiveDocument() && documents_.size() == 1 && !tabDirty(activeDocument()) && table().filename().empty() && activeDocument().resourceIdentity.empty() && table().rowCount() == 1 && table().columnCount() == 1;
    }

    void ensureDocumentTabForOpen() {
        if(hasActiveDocument())commitPendingCell();
        if (!hasActiveDocument()) {
            createDocumentTab(false);
            return;
        }
        if (!activeTabIsReusableForOpen()) createDocumentTab(false);
    }

#if defined(__EMSCRIPTEN__)
    using BrowserImportCallback = std::function<void(neobrowser::BrowserImportLease)>;

    void requestBrowserImport(const std::string& title,
                              const std::string& accept,
                              bool multiple,
                              BrowserImportCallback callback) {
        wxWeakRef<Neo2DAPanelImpl> weakSelf(this);
        neobrowser::requestOpenFilesOwned(
            title, accept, multiple,
            [weakSelf, callback = std::move(callback)](
                neobrowser::OwnedOpenFilesResult result) mutable {
                if (!weakSelf || weakSelf->IsBeingDeleted()) return;
                auto* const frame = weakSelf.get();
                if (!result.error.empty()) {
                    wxMessageBox(wxui::toWx(result.error), "File Open Error",
                                 wxOK | wxICON_ERROR, frame);
                    return;
                }
                if (result.cancelled()) return;
                callback(std::move(result.import));
            });
    }

    static bool importOwnsPath(const neobrowser::BrowserImportLease& import,
                               const std::filesystem::path& path) {
        return std::find(import.paths().begin(), import.paths().end(), path) !=
               import.paths().end();
    }
#endif

    void commitPendingCell() {
        if(grid_ && grid_->IsCellEditControlShown()) {
            grid_->SaveEditControlValue();grid_->HideCellEditControl();grid_->DisableCellEditControl();
        }
    }
    bool confirmCloseDocumentTab(std::size_t index) {
        if(index==activeDocumentIndex_)commitPendingCell();
        if (index >= documents_.size()) return true;
        if (documents_[index].saveInProgress) {
            wxui::showMessage(this, "Save in progress",
                              "Finish the browser save transaction before closing this tab.");
            return false;
        }
        if (!tabDirty(documents_[index])) return true;
        const auto previous=activeDocumentIndex_;
        selectDocumentTab(index);
        const int answer=wxMessageBox(wxui::toWx("Save changes to "+tabDisplayName(documents_[index])+"?"),
            "Close table",wxYES_NO|wxCANCEL|wxICON_QUESTION,this);
        bool allowed=answer==wxNO;
        if(answer==wxYES) {
            try {allowed=table().filename().empty()?saveAs():saveTo(table().filename());}
            catch(const std::exception& ex){wxui::showError(this,ex);allowed=false;}
        }
        if(previous<documents_.size())selectDocumentTab(previous);
        return allowed;
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

    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;

    void buildMenus() {
        auto* file = new wxMenu;
        file->Append(ID_New, "&New 2DA");
        file->Append(ID_Open, context_.embedded?"&Open 2DA/GDA...\tCtrl+Shift+O":"&Open 2DA/GDA...\tCtrl+O");
        recentFilesMenu_ = new wxMenu;
        rebuildRecentFilesMenu();
        file->AppendSubMenu(recentFilesMenu_, "Open &Recent");
        file->Append(ID_Save, "&Save\tCtrl+S");
        file->Append(ID_SaveAs, "Save &As...\tCtrl+Shift+S");
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
        if(!context_.embedded) file->Append(ID_ModuleExit, "E&xit");

        auto* import = new wxMenu;
        import->Append(ID_ImportCsv, "Import &CSV...");
        import->Append(ID_ImportTsv, "Import &TSV...");

        auto* exportMenu = new wxMenu;
        exportMenu->Append(ID_ExportCsv, "Export as &CSV...");
        exportMenu->Append(ID_ExportTsv, "Export as &TSV...");

        auto* tools = new wxMenu;
        tools->Append(ID_GeneratePatcher, "Generate TSLPatcher/HoloPatcher Instructions...");

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
        if(!context_.embedded) {
        darkModeItem_ = view->AppendCheckItem(ID_DarkMode, "&Dark Mode");
        view->AppendSeparator();
        view->Append(ID_FontIncrease, "Increase Font Size\tCtrl++");
        view->Append(ID_FontDecrease, "Decrease Font Size\tCtrl+-");
        view->Append(ID_FontReset, "Reset Font Size\tCtrl+0");
        }

        auto* help = new wxMenu;
        help->Append(ID_ModuleAbout, "&About");

        auto* bar = new wxMenuBar;
        bar->Append(file, "&File");
        bar->Append(import, "&Import");
        bar->Append(exportMenu, "&Export");
        bar->Append(tools, "&Tools");
        bar->Append(edit, "&Edit");
        bar->Append(view, "&View");
        bar->Append(help, "&Help");
        setModuleMenus(bar);

        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onNew, this, ID_New);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onOpen, this, ID_Open);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onOpenRecent, this, kRecentFileBaseId, kRecentFileBaseId + neosettings::kMaxRecentFiles - 1);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onClearRecentFiles, this, kClearRecentFilesId);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onSave, this, ID_Save);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onCloseTab, this, ID_CloseTab);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onCloseOtherTabs, this, ID_CloseOtherTabs);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onNextTab, this, ID_NextTab);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onPreviousTab, this, ID_PreviousTab);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onAddRow, this, ID_AddRow);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onAddColumn, this, ID_AddColumn);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onCloneRow, this, ID_CloneRow);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onDeleteRow, this, ID_DeleteRow);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onDeleteColumn, this, ID_DeleteColumn);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onRenameRow, this, ID_RenameRow);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onRenameColumn, this, ID_RenameColumn);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onCopyCells, this, ID_CopyCells);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onPasteCells, this, ID_PasteCells);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onFilterPrompt, this, ID_Filter);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onClearFilter, this, ID_ClearFilter);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onFilterSelectedColumn, this, ID_FilterColumn);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onClearSelectedColumnFilter, this, ID_ClearColumnFilter);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onClearAllFilters, this, ID_ClearAllFilters);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onMoveColumnLeft, this, ID_MoveColumnLeft);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onMoveColumnRight, this, ID_MoveColumnRight);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onResetColumnOrder, this, ID_ResetColumnOrder);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onResetRowOrder, this, ID_ResetRowOrder);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(neotabular::Format::Csv); }, ID_ImportCsv);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(neotabular::Format::Tsv); }, ID_ImportTsv);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(neotabular::Format::Csv); }, ID_ExportCsv);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(neotabular::Format::Tsv); }, ID_ExportTsv);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onGeneratePatcherOutput(); }, ID_GeneratePatcher);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onToggleDarkMode, this, ID_DarkMode);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &Neo2DAPanelImpl::onResetFontScale, this, ID_FontReset);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestModuleClose(); }, ID_ModuleExit);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            wxui::showMessage(this, "About Neo2DA", std::string("Neo2DA v") + kVersion + "\nNative wxWidgets 2DA table editor\n\nA special thanks to everyone in the KOTOR modding community that has contributed their work, knowledge, and creativity to making tools, mods, and guides over the last 20+ years");
        }, ID_ModuleAbout);
        Bind(wxEVT_BUTTON, &Neo2DAPanelImpl::onOpen, this, ID_Open);
        Bind(wxEVT_BUTTON, &Neo2DAPanelImpl::onSave, this, ID_Save);
        Bind(wxEVT_BUTTON, &Neo2DAPanelImpl::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_BUTTON, &Neo2DAPanelImpl::onAddRow, this, ID_AddRow);
        Bind(wxEVT_BUTTON, &Neo2DAPanelImpl::onAddColumn, this, ID_AddColumn);
        Bind(wxEVT_BUTTON, &Neo2DAPanelImpl::onCloneRow, this, ID_CloneRow);
        Bind(wxEVT_BUTTON, &Neo2DAPanelImpl::onDeleteRow, this, ID_DeleteRow);
        Bind(wxEVT_BUTTON, &Neo2DAPanelImpl::onDeleteColumn, this, ID_DeleteColumn);
        Bind(wxEVT_BUTTON, &Neo2DAPanelImpl::onRenameRow, this, ID_RenameRow);
        Bind(wxEVT_BUTTON, &Neo2DAPanelImpl::onRenameColumn, this, ID_RenameColumn);
        Bind(wxEVT_BUTTON, &Neo2DAPanelImpl::onClearFilter, this, ID_ClearFilter);
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
        auto* layout=new wxBoxSizer(wxVERTICAL);
        layout->Add(panel,1,wxEXPAND);
        SetSizer(layout);
        grid_->SetName("Neo2DA grid");
        filterText_->SetName("Neo2DA search");
        documentTabs_->SetName("Neo2DA documents");

        filterText_->Bind(wxEVT_TEXT, &Neo2DAPanelImpl::onFilterText, this);
        grid_->Bind(wxEVT_GRID_CELL_CHANGED, &Neo2DAPanelImpl::onCellChanged, this);
        grid_->Bind(wxEVT_GRID_LABEL_LEFT_DCLICK, &Neo2DAPanelImpl::onLabelDoubleClicked, this);
        grid_->Bind(wxEVT_GRID_LABEL_RIGHT_CLICK, &Neo2DAPanelImpl::onGridLabelRightClick, this);
        grid_->Bind(wxEVT_GRID_COL_MOVE, &Neo2DAPanelImpl::onGridColumnMoved, this);
        grid_->Bind(wxEVT_GRID_SELECT_CELL, &Neo2DAPanelImpl::onCellSelected, this);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &Neo2DAPanelImpl::onDocumentTabChanged, this);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &Neo2DAPanelImpl::onDocumentTabCloseRequested, this);
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
        if(path.empty())return false;
        TwoDAFile loaded(path); // Parse before replacing/creating a document.
        const auto normalized=neosettings::normalizedPath(path);
        for(std::size_t i=0;i<documents_.size();++i) {
            if(!documents_[i].table->filename().empty() && neosettings::normalizedPath(documents_[i].table->filename())==normalized) {
                selectDocumentTab(i);return true;
            }
        }
        if(checkDirty)ensureDocumentTabForOpen();
        if(!hasActiveDocument())createDocumentTab(false);
        table()=std::move(loaded);
        activeDocument().resourceIdentity.clear();activeDocument().sourceDescription.clear();
        activeDocument().protectedInputs.clear();
        viewState().resetForNewDocument();setFilterTerm({});
        viewState().selectedVisualRow=0;viewState().selectedVisualColumn=0;
        refreshGrid();updateStatus();rememberRecentFile(path);
        neogames::resolver().inferFromOpenedPath(path);
        return true;
    }

#if defined(__EMSCRIPTEN__)
    bool openTablePath(const std::filesystem::path& path,
                       neobrowser::BrowserImportLease import,
                       bool checkDirty) {
        if (!openTablePath(path, checkDirty)) return false;
        activeDocument().sourceImport = std::move(import);
        return true;
    }
#endif

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
        filePath_->ChangeValue(wxui::toWx(table().filename().empty() ?
            activeDocument().sourceDescription : pathText(table().filename())));
        const std::string name = tabDisplayName(activeDocument());
        const std::string state = activeDocument().saveInProgress
            ? " saving..."
            : (table().dirty() ? " modified" : "");
        setModuleStatusText( wxui::toWx(name + state), 0);
        if (table().loaded()) {
            std::string detail = std::to_string(viewState().visualToLogicalRows.size()) + "/" +
                                 std::to_string(table().rowCount()) + " rows, " +
                                 std::to_string(table().columnCount()) + " columns";
            const std::string columnFilters = neoview::columnFilterSummary(viewState());
            if (!columnFilters.empty()) detail += "; filters: " + columnFilters;
            setModuleStatusText( wxui::toWx(detail), 1);
        } else {
            setModuleStatusText( "No table", 1);
        }
    }

    bool saveTo(const std::filesystem::path& path) {
        if (path.empty() || !hasActiveDocument()) return false;
        if (activeDocument().saveInProgress || browserSaveActive_) return false;

        validateHostOutput(path);
        DocumentTab& document = activeDocument();
        const bool wasDirty = document.table->dirty();
        if(!document.resourceIdentity.empty()) {
            neo2da::checkDetachedDestination(path,document.protectedInputs);
        }
        for(const auto& other:documents_) {
            if(&other==&document || other.table->filename().empty())continue;
            std::error_code ec;
            const bool alias=std::filesystem::equivalent(path,other.table->filename(),ec);
            if(neosettings::normalizedPath(path)==neosettings::normalizedPath(other.table->filename()) || (!ec && alias))
                throw TwoDAError("That destination is open in another tab. Save to a different file or close the other tab first.");
        }
        commitPendingCell();
        document.table->save(path);

#if defined(__EMSCRIPTEN__)
        document.saveInProgress = true;
        browserSaveActive_ = true;
        updateDocumentTabTitle(document);
        updateStatus();
        Enable(false);

        wxWeakRef<Neo2DAPanelImpl> weakSelf(this);
        wxWindow* const targetPage = document.tabPage;
        neobrowser::requestDownloadFile(
            path,
            path.filename().string(),
            [weakSelf, targetPage, path, wasDirty](neobrowser::DownloadResult result) {
                if (!weakSelf || weakSelf->IsBeingDeleted()) return;
                auto* const frame = weakSelf.get();
                frame->browserSaveActive_ = false;
                frame->Enable(true);

                const std::size_t index = neotabs::findDocumentIndexForPage(
                    frame->documents_, targetPage);
                if (index == neotabs::npos) return;

                DocumentTab& savedDocument = frame->documents_[index];
                savedDocument.saveInProgress = false;
                if (!result.error.empty() || result.cancelled()) {
                    savedDocument.table->setDirty(wasDirty);
                    frame->updateDocumentTabTitle(savedDocument);
                    if (index == frame->activeDocumentIndex_) frame->updateStatus();
                    const std::string message = result.error.empty()
                        ? "The browser save transaction was cancelled."
                        : result.error;
                    wxMessageBox(wxui::toWx(message), "Save Failed",
                                 wxOK | wxICON_ERROR, frame);
                    return;
                }

                savedDocument.table->setFilename(path);
                savedDocument.table->setDirty(false);
                if (!frame->importOwnsPath(savedDocument.sourceImport, path)) {
                    savedDocument.sourceImport.reset();
                }
                frame->rememberRecentFile(path);
                neogames::resolver().inferFromOpenedPath(path);
                frame->updateDocumentTabTitle(savedDocument);
                if (index == frame->activeDocumentIndex_) frame->updateStatus();

                if (result.ready()) {
                    wxui::showMessage(
                        frame,
                        "Replacement download ready",
                        "The browser could not overwrite the original host file directly. "
                        "A replacement file is ready in the download panel; download it before closing this page.");
                }
            });
        return true;
#else
        document.table->setFilename(path);
        document.table->setDirty(false);
        rememberRecentFile(path);
        neogames::resolver().inferFromOpenedPath(path);
        updateStatus();
        return true;
#endif
    }

    bool saveAs() {
        const std::string defaultName = table().filename().empty() ? (activeDocument().resourceIdentity.empty()?"new.2da":activeDocument().untitledName) : table().filename().filename().string();
        const auto chosen = wxui::chooseSaveFile(this, "Save 2DA/GDA as", k2DAWildcard, defaultName);
        if (!chosen) {
            return false;
        }
        return saveTo(*chosen);
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

    void generatePatcherOutputFromOriginal(const std::filesystem::path& originalPath) {
        try {
            if (table().isGda()) {
                throw std::runtime_error(
                    "TSLPatcher/HoloPatcher [2DAList] supports KotOR-style 2DA files only. "
                    "Dragon Age GDA files can be edited and exported as CSV/TSV, but cannot be emitted as 2DA patch instructions.");
            }
            TwoDAFile original(originalPath);
            if (original.isGda()) {
                throw std::runtime_error("The selected baseline is a Dragon Age GDA file; choose a KotOR-style 2DA file.");
            }

            const auto output = wxui::choosePatcherOutput(this);
            if (!output) return;
            const bool writeToIni = output->writesToIni();

            const std::string patchFilename = originalPath.filename().string().empty() ? std::string("table.2da") : originalPath.filename().string();
            auto project = neotsl::diffTwoDA(original.toTable(), table().toTable(), patchFilename, writeToIni, originalPath);
            neotsl::throwIfUnsupported(project);

            if (!writeToIni) {
                wxui::showIniFragmentDialog(
                    this,
                    "2DA Patcher INI Fragment",
                    project,
                    {patchFilename});
                return;
            }

            neo2da::checkProtectedOutput(output->iniPath,activeDocument().protectedInputs);
            const auto report = neotsl::writePackageToIni(project, output->iniPath, true);
            wxui::showMessage(
                this,
                "Patcher Package Generated",
                std::string(report.mergedExisting ? "Merged the generated 2DA instructions into:\n"
                                                  : "Created the installer INI:\n") +
                    neosettings::pathToUtf8(report.iniPath) +
                    "\n\nRequired package files were staged beside the selected INI.");
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onGeneratePatcherOutput() {
        try {
            if (table().isGda()) {
                throw std::runtime_error(
                    "TSLPatcher/HoloPatcher [2DAList] supports KotOR-style 2DA files only. "
                    "Dragon Age GDA files can be edited and exported as CSV/TSV, but cannot be emitted as 2DA patch instructions.");
            }
#if defined(__EMSCRIPTEN__)
            wxWindow* const targetPage = activeDocument().tabPage;
            requestBrowserImport(
                "Choose original/base KotOR 2DA",
                ".2da",
                false,
                [this, targetPage](neobrowser::BrowserImportLease import) {
                    if (import.empty() || IsBeingDeleted()) return;
                    if (!hasActiveDocument() || activeDocument().tabPage != targetPage) {
                        wxui::showMessage(
                            this,
                            "Patcher Export Cancelled",
                            "The active document changed while the baseline picker was open. Start the export again from the intended tab.");
                        return;
                    }
                    generatePatcherOutputFromOriginal(import.paths().front());
                });
#else
            const auto originalPath = wxui::chooseOpenFile(
                this,
                "Choose original/base KotOR 2DA",
                "KotOR 2DA files (*.2da)|*.2da|All files (*.*)|*.*");
            if (!originalPath) return;
            generatePatcherOutputFromOriginal(*originalPath);
#endif
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void importFromPath(neotabular::Format format, const std::filesystem::path& chosen) {
        try {
            const neotabular::Table imported = neotabular::readTable(chosen, format);
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

    void onImport(neotabular::Format format) {
#if defined(__EMSCRIPTEN__)
        wxWindow* const targetPage = activeDocument().tabPage;
        requestBrowserImport(
            "Import " + neotabular::formatName(format),
            "." + exportExtensionForFormat(format),
            false,
            [this, targetPage, format](neobrowser::BrowserImportLease import) {
                if (import.empty() || IsBeingDeleted()) return;
                if (!hasActiveDocument() || activeDocument().tabPage != targetPage) {
                    wxui::showMessage(
                        this,
                        "Import Cancelled",
                        "The active document changed while the file picker was open. Start the import again from the intended tab.");
                    return;
                }
                importFromPath(format, import.paths().front());
            });
#else
        try {
            const auto chosen = wxui::chooseOpenFile(
                this,
                "Import " + neotabular::formatName(format),
                wildcardForFlatFormat(format));
            if (!chosen) return;
            importFromPath(format, *chosen);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
#endif
    }

    void onExport(neotabular::Format format) {
        try {
            const auto chosen = wxui::chooseSaveFile(this, "Export " + neotabular::formatName(format), wildcardForFlatFormat(format),
                                                   exportDefaultFilename(table().filename(), format, "table"));
            if (!chosen) return;
            validateHostOutput(*chosen);
            neo2da::checkProtectedOutput(*chosen,activeDocument().protectedInputs);
            commitPendingCell();
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
#if defined(__EMSCRIPTEN__)
        (void)initialDirectory;
        requestBrowserImport(
            "Open 2DA/GDA (optionally select gda_column_names.tsv or .csv with a GDA)",
            kBrowserTableAccept,
            true,
            [this](neobrowser::BrowserImportLease import) {
                if (import.empty() || IsBeingDeleted()) return;
                try {
                    const std::filesystem::path chosen = neo2da::browseropen::selectTablePath(import.paths());
                    openTablePath(chosen, std::move(import), true);
                } catch (const std::exception& ex) {
                    wxui::showError(this, ex);
                }
            });
#else
        try {
            const auto chosen = wxui::chooseOpenFile(this, "Open 2DA/GDA", k2DAWildcard, initialDirectory);
            if (!chosen) return;
            openTablePath(*chosen, true);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
#endif
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
    bool browserSaveActive_ = false;
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    double fontScale_ = neoview::kDefaultFontScale;
    bool darkMode_ = false;
};

 } // namespace
namespace neo2da::ui {
EditorPanel* createEditorPanel(wxWindow* parent, neomodules::Context context) {
    return new Neo2DAPanelImpl(parent, std::move(context));
}
}
