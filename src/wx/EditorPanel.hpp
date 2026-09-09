#pragma once
#include "NeoModulePanel.hpp"
#include "core/TwoDAFile.hpp"
#include <neoshared/ResourceDocument.hpp>
namespace neo2da::ui {
inline constexpr unsigned kEditorApiVersion=1;
class EditorPanel : public neomodules::Panel {
public:
    using Panel::Panel;
    virtual bool openFile(const std::filesystem::path& path)=0;
    virtual bool openResource(neoshared::ResourceDocument resource)=0;
    virtual bool saveActiveAs(const std::filesystem::path& path)=0;
    virtual bool activateResource(const std::string& identity)=0;
    virtual std::size_t documentCount() const=0;
    virtual TwoDAFile* activeTable()=0;
    virtual void refreshActiveTable()=0;
};
EditorPanel* createEditorPanel(wxWindow* parent, neomodules::Context context = {});
} // namespace neo2da::ui
