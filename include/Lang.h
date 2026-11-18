#pragma once
#include "SKSEMenuFramework.h"
#include <cstdarg>
#include <format>
#include <string>
#include <vector>

// 2.5v: translations. Every piece of menu text goes through here; the English text itself is the key, so a new
// feature never breaks a translation (its new lines just stay English until someone translates them).
// Files: Data/SKSE/Plugins/AnatomicaMenu/Translations/<name>.ini, pairs of lines
//     en: Belly bulge
//     tr: Bauchwölbung
// The language is picked on the General page (or follows the game's language), kept in Translations/Language.ini.
namespace Lang {
    void Init();                                   // reads the chosen language (before the menu pages are added)
    void Refresh();                                // "game language": look again once the game has read its INIs
    bool Active();                                 // a translation is in use
    const char* T(const char* en);                 // display text (shown as it is)
    const char* F(const char* fmt);                // a printf format (only taken when its %s / %d... match the English)
    const char* L(const char* label);              // a widget label: the translation, keeping the English as its ID
    std::string TS(const std::string& en);         // T() for a std::string
    // 2.5zb: a std::format line ("Wet look: {} beads ..."): the translation is used when it formats, else the English
    template <class... A> std::string Fmt(const char* en, const A&... a) {
        const char* t = T(en);
        if (t != en) { try { return std::vformat(t, std::make_format_args(a...)); } catch (...) {} }
        try { return std::vformat(en, std::make_format_args(a...)); } catch (...) { return en; }
    }

    struct Option { std::string file, name; };     // file "" = English, "*" = the game's language
    std::vector<Option> Options();
    std::string Current();                         // the chosen file ("" / "*" / a name)
    std::string LoadedName();                      // what is in use now ("English" when none)
    int Count();                                   // lines translated in the file in use
    bool Choose(const std::string& file);          // switch now (page names change after a restart)
    std::string WriteTemplate();                   // writes _template.ini (and <name>_updated.ini); returns what it did
    void RenderGeneral();                          // the Language section on the General page
}

// The ImGui calls Anatomica uses, translating their text on the way (same names and arguments as ImGuiMCP).
namespace Tr {
    using namespace ImGuiMCP;
    inline bool Checkbox(const char* label, bool* v) { return ImGuiMCP::Checkbox(Lang::L(label), v); }
    inline bool Button(const char* label, const ImVec2 size = ImVec2(0, 0)) { return ImGuiMCP::Button(Lang::L(label), size); }
    inline bool SmallButton(const char* label) { return ImGuiMCP::SmallButton(Lang::L(label)); }
    inline bool SliderFloat(const char* label, float* v, float a, float b, const char* format = "%.3f", ImGuiSliderFlags flags = 0) {
        return ImGuiMCP::SliderFloat(Lang::L(label), v, a, b, format ? Lang::F(format) : format, flags);
    }
    inline bool SliderInt(const char* label, int* v, int a, int b, const char* format = "%d", ImGuiSliderFlags flags = 0) {
        return ImGuiMCP::SliderInt(Lang::L(label), v, a, b, format ? Lang::F(format) : format, flags);
    }
    inline bool ColorEdit3(const char* label, float col[3], ImGuiColorEditFlags flags = 0) { return ImGuiMCP::ColorEdit3(Lang::L(label), col, flags); }
    inline bool TreeNode(const char* label) { return ImGuiMCP::TreeNode(Lang::L(label)); }
    inline bool TreeNode(const char* id, const char* fmt, ...) {
        char buf[512];
        va_list a; va_start(a, fmt);
        std::vsnprintf(buf, sizeof buf, Lang::F(fmt), a);
        va_end(a);
        return ImGuiMCP::TreeNode(id, "%s", buf);
    }
    inline bool CollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0) { return ImGuiMCP::CollapsingHeader(Lang::L(label), flags); }
    inline bool InputText(const char* label, char* buf, size_t n, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback cb = NULL, void* ud = NULL) {
        return ImGuiMCP::InputText(Lang::L(label), buf, n, flags, cb, ud);
    }
    inline bool Selectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0, const ImVec2 size = ImVec2(0, 0)) {
        return ImGuiMCP::Selectable(Lang::L(label), selected, flags, size);
    }
    inline bool Selectable(const char* label, bool* p, ImGuiSelectableFlags flags = 0, const ImVec2 size = ImVec2(0, 0)) {
        return ImGuiMCP::Selectable(Lang::L(label), p, flags, size);
    }
    inline bool BeginCombo(const char* label, const char* preview, ImGuiComboFlags flags = 0) {
        return ImGuiMCP::BeginCombo(Lang::L(label), preview ? Lang::T(preview) : preview, flags);
    }
    inline bool Combo(const char* label, int* cur, const char* const items[], int count, int h = -1) {
        if (!Lang::Active()) return ImGuiMCP::Combo(label, cur, items, count, h);
        std::vector<const char*> t(static_cast<size_t>(count > 0 ? count : 0));
        for (int i = 0; i < count; ++i) t[i] = Lang::T(items[i]);
        return ImGuiMCP::Combo(Lang::L(label), cur, t.data(), count, h);
    }
    inline bool Begin(const char* name, bool* open = NULL, ImGuiWindowFlags flags = 0) { return ImGuiMCP::Begin(Lang::L(name), open, flags); }
    inline void SeparatorText(const char* label) { ImGuiMCP::SeparatorText(Lang::T(label)); }
    inline void ProgressBar(float f, const ImVec2 size = ImVec2(-FLT_MIN, 0), const char* overlay = NULL) { ImGuiMCP::ProgressBar(f, size, overlay ? Lang::T(overlay) : overlay); }
    inline void TextUnformatted(const char* text, const char* end = NULL) { ImGuiMCP::TextUnformatted(end ? text : Lang::T(text), end); }
    // "%s" alone: the text passed is what gets translated (Text("%s", ok ? "Yes" : "No"))
    inline const char* Arg(const char* fmt, va_list a) { return (Lang::Active() && fmt && fmt[0] == '%' && fmt[1] == 's' && !fmt[2]) ? Lang::T(va_arg(a, const char*)) : nullptr; }
#define ANAT_TR_V(CALL_S, CALL_V)                       \
        va_list a; va_start(a, fmt);                    \
        va_list b; va_copy(b, a);                       \
        if (const char* s = Arg(fmt, b)) CALL_S;        \
        else CALL_V;                                    \
        va_end(b); va_end(a);
    inline void Text(const char* fmt, ...) { ANAT_TR_V(ImGuiMCP::TextUnformatted(s), ImGuiMCP::TextV(Lang::F(fmt), a)) }
    inline void TextWrapped(const char* fmt, ...) { ANAT_TR_V(ImGuiMCP::TextWrapped("%s", s), ImGuiMCP::TextWrappedV(Lang::F(fmt), a)) }
    inline void TextDisabled(const char* fmt, ...) { ANAT_TR_V(ImGuiMCP::TextDisabled("%s", s), ImGuiMCP::TextDisabledV(Lang::F(fmt), a)) }
    inline void TextColored(const ImVec4 col, const char* fmt, ...) { ANAT_TR_V(ImGuiMCP::TextColored(col, "%s", s), ImGuiMCP::TextColoredV(col, Lang::F(fmt), a)) }
    inline void BulletText(const char* fmt, ...) { ANAT_TR_V(ImGuiMCP::BulletText("%s", s), ImGuiMCP::BulletTextV(Lang::F(fmt), a)) }
    inline void SetTooltip(const char* fmt, ...) { ANAT_TR_V(ImGuiMCP::SetTooltip("%s", s), ImGuiMCP::SetTooltipV(Lang::F(fmt), a)) }
#undef ANAT_TR_V
}
