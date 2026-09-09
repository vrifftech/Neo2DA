#include "EditorPanel.hpp"
#include "core/Version.hpp"
#include "neo2da_icon.xpm"
#include "NeoSettings.hpp"
#include <wx/app.h>
#include <wx/iconbndl.h>
namespace {
class Neo2DAFrame final : public wxFrame {
public:
    Neo2DAFrame():wxFrame(nullptr,wxID_ANY,wxui::toWx(std::string("Neo2DA v")+neo2da::kVersion)) {
        wxIconBundle icons;
#if defined(__WXMSW__)
        wxIcon native("neo2da",wxBITMAP_TYPE_ICO_RESOURCE);if(native.IsOk())icons.AddIcon(native);
#endif
        wxIcon fallback(neo2da_icon_xpm);if(fallback.IsOk())icons.AddIcon(fallback);
        SetIcons(icons);
        neomodules::Context context;
        context.titleChanged=[this](const wxString& title){SetTitle(title);};
        context.closeRequested=[this]{Close();};
        panel_=neo2da::ui::createEditorPanel(this,std::move(context));
        SetMenuBar(panel_->takeMenus().release());
        auto* layout=new wxBoxSizer(wxVERTICAL);layout->Add(panel_,1,wxEXPAND);SetSizer(layout);
        Bind(wxEVT_MENU,[this](wxCommandEvent& event){if(!neomodules::routeCommand({panel_},event))event.Skip();});
        Bind(wxEVT_MENU_OPEN,[this](wxMenuEvent& event){neomodules::routeMenuOpen({panel_},event);event.Skip();});
        Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent& event){
            if(!panel_->canClose()){if(event.CanVeto()){event.Veto();return;}}
            settings_.saveWindowPlacement(*this);event.Skip();
        });
        wxui::configureResponsiveWindow(*this,wxSize(1050,720),wxSize(620,420));
        settings_.restoreWindowPlacement(*this);
    }
    ~Neo2DAFrame() override {DestroyChildren();}
    void openStartup(const std::filesystem::path& path){try{panel_->openFile(path);}catch(const std::exception& ex){wxui::showError(this,ex);}}
private:
    neo2da::ui::EditorPanel* panel_{};
    neosettings::AppSettings settings_{"Neo2DA"};
};
class Neo2DAApp final:public wxApp {
public:bool OnInit()override {
    SetAppName("Neo2DA");SetVendorName("Neo Tools");wxInitAllImageHandlers();
    auto* frame=new Neo2DAFrame;frame->Show();
    if(argc>1){const auto path=neosettings::pathFromWx(wxString(argv[1]));frame->CallAfter([frame,path]{frame->openStartup(path);});}
    return true;
}};
}
wxIMPLEMENT_APP(Neo2DAApp);
