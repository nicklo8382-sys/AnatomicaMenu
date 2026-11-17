#include "Lang.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <unordered_map>

namespace {
#include "LangKeys.inc"   // kKeys: every piece of English menu text (made from the source by tools/langkeys.py)

    const std::filesystem::path kDir = "Data/SKSE/Plugins/AnatomicaMenu/Translations";
    const std::filesystem::path kChoice = kDir / "Language.ini";

    struct SvHash { using is_transparent = void; size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); } };
    std::unordered_map<std::string, std::string, SvHash, std::equal_to<>> g_tr;    // English -> translation
    std::unordered_map<std::string, std::string, SvHash, std::equal_to<>> g_fmt;   // the same, only where it is a safe format
    std::unordered_map<std::string, std::string, SvHash, std::equal_to<>> g_lbl;   // full label -> translation + its ID
    std::string g_choice = "*";      // "" English, "*" the game's language, else a file name (no .ini)
    std::string g_loaded;            // the file in use ("" = English)
    std::string g_loadedName = "English";
    std::string g_problem;           // why the last file could not be used (menu)
    int g_bad = 0;                   // lines skipped because their %s / %d... did not match the English

    std::string Lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
    std::string Unescape(std::string_view s) {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                const char n = s[++i];
                o += n == 'n' ? '\n' : n == 't' ? '\t' : n;   // \\ -> backslash, \" -> "
            } else o += s[i];
        }
        return o;
    }
    std::string Escape(std::string_view s) {
        std::string o;
        for (char c : s) {
            if (c == '\n') o += "\\n";
            else if (c == '\t') o += "\\t";
            else if (c == '\\') o += "\\\\";
            else o += c;
        }
        return o;
    }
    std::string Trim(std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        return s.substr(i);
    }
    // the printf conversions in a string, in order ("%%" is not one); "!" when it has one we never allow (%n)
    std::string Specs(std::string_view s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '%') continue;
            if (i + 1 < s.size() && s[i + 1] == '%') { ++i; continue; }
            size_t j = i + 1;
            while (j < s.size() && std::string_view("-+ #0").find(s[j]) != std::string_view::npos) ++j;
            while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '*')) ++j;
            if (j < s.size() && s[j] == '.') { ++j; while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '*')) ++j; }
            std::string len;
            while (j < s.size() && std::string_view("hlLzjtI").find(s[j]) != std::string_view::npos) len += s[j++];
            if (j >= s.size()) continue;
            const char c = s[j];
            if (c == 'n') return "!";
            if (std::string_view("diouxXfFeEgGaAcsp").find(c) == std::string_view::npos) continue;   // "100% =" is just text
            // what is passed decides what is safe: ints, doubles, pointers/strings
            const char kind = std::string_view("diouxXc").find(c) != std::string_view::npos ? 'i' : (c == 's' || c == 'p') ? 's' : 'f';
            out += kind;
            out += len;
            out += (s.substr(i, j - i).find('*') != std::string_view::npos) ? "*" : "";
            out += ',';
            i = j;
        }
        return out;
    }
    // the game's language (sLanguage): from the game once it has read its INIs, before that from Skyrim.ini itself
    std::string GameLanguage() {
        if (auto* col = RE::INISettingCollection::GetSingleton())
            if (auto* s = col->GetSetting("sLanguage:General"); s && s->GetType() == RE::Setting::Type::kString && s->GetString() && *s->GetString())
                return s->GetString();
        std::vector<std::filesystem::path> tries;
        for (const char* env : {"USERPROFILE", "OneDrive"})
            if (const char* v = std::getenv(env); v && *v) {
                tries.push_back(std::filesystem::path(v) / "Documents/My Games/Skyrim Special Edition/Skyrim.ini");
                tries.push_back(std::filesystem::path(v) / "Documents/My Games/Skyrim Special Edition GOG/Skyrim.ini");
            }
        for (auto& p : tries) {
            std::ifstream in(p, std::ios::binary);
            std::string line;
            while (in && std::getline(in, line)) {
                line = Trim(line);
                if (Lower(line).rfind("slanguage", 0) == 0) if (auto eq = line.find('='); eq != std::string::npos) return Trim(line.substr(eq + 1));
            }
        }
        return {};
    }
    std::filesystem::path FileFor(const std::string& choice) {
        std::string name = choice;
        if (name == "*") {
            name = GameLanguage();
            if (Lower(name) == "english") name.clear();
        }
        if (name.empty()) return {};
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(kDir, ec))
            if (e.is_regular_file(ec) && Lower(e.path().extension().string()) == ".ini" && Lower(e.path().stem().string()) == Lower(name)) return e.path();
        return {};
    }
    std::string NameIn(const std::filesystem::path& f) {   // "name: Deutsch" at the top, else the file name
        std::ifstream in(f, std::ios::binary);
        std::string line;
        for (int n = 0; n < 20 && std::getline(in, line); ++n) {
            if (n == 0 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
            line = Trim(line);
            if (line.rfind("name:", 0) == 0) { auto v = Trim(line.substr(5)); if (!v.empty()) return v; }
        }
        return f.stem().string();
    }
    void Load(const std::filesystem::path& f) {
        g_tr.clear(); g_fmt.clear(); g_lbl.clear(); g_bad = 0; g_problem.clear();
        g_loaded.clear(); g_loadedName = "English";
        if (f.empty()) {
            if (g_choice != "" && g_choice != "*") g_problem = std::format("{}.ini was not found in the Translations folder - a file added while the game runs is only seen after a restart", g_choice);
            return;
        }
        std::ifstream in(f, std::ios::binary);
        if (!in) { g_problem = "could not open " + f.filename().string() + " - restart the game: a file added or renamed while it runs can't be opened until then (Mod Organizer)"; return; }
        std::string line, en;
        bool first = true, haveEn = false;
        while (std::getline(in, line)) {
            if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
            first = false;
            while (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string t = Trim(line);
            if (t.rfind("en:", 0) == 0) { en = Unescape(Trim(t.substr(3))); haveEn = true; continue; }
            if (t.rfind("tr:", 0) == 0 && haveEn) {
                haveEn = false;
                std::string tr = Unescape(Trim(t.substr(3)));
                if (tr.empty() || en.empty()) continue;
                const std::string se = Specs(en), st = Specs(tr);
                g_tr[en] = tr;                       // shown as it is: always safe
                if (se == st && st != "!") g_fmt[en] = tr;
                else ++g_bad;                        // used as text only, never as a format
            }
        }
        g_loaded = f.stem().string();
        g_loadedName = NameIn(f);
        logger::info("Anatomica: language {} ({} lines{})", g_loadedName, g_tr.size(),
                     g_bad ? std::format(", {} with %s / %d that do not match the English - those stay English where a number is shown", g_bad) : "");
    }
    void SaveChoice() {
        std::error_code ec;
        std::filesystem::create_directories(kDir, ec);
        std::ofstream o(kChoice, std::ios::binary | std::ios::trunc);
        o << "; which translation Anatomica's menu uses: * = the game's language, empty = English, else a file name here (no .ini)\n";
        o << "Language=" << g_choice << "\n";
    }
}

namespace Lang {
    void Init() {
        std::ifstream in(kChoice, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            line = Trim(line);
            if (Lower(line).rfind("language=", 0) == 0) g_choice = Trim(line.substr(9));
        }
        Load(FileFor(g_choice));
    }
    void Refresh() { if (g_choice == "*") Load(FileFor(g_choice)); }   // once the game has read its INIs
    bool Active() { return !g_tr.empty(); }
    const char* T(const char* en) {
        if (!en || g_tr.empty()) return en;
        auto it = g_tr.find(std::string_view(en));
        return it == g_tr.end() ? en : it->second.c_str();
    }
    const char* F(const char* fmt) {
        if (!fmt || g_fmt.empty()) return fmt;
        auto it = g_fmt.find(std::string_view(fmt));
        return it == g_fmt.end() ? fmt : it->second.c_str();
    }
    const char* L(const char* label) {
        if (!label || g_tr.empty()) return label;
        const std::string_view full(label);
        if (auto c = g_lbl.find(full); c != g_lbl.end()) return c->second.c_str();
        const size_t cut = full.find("##");
        const std::string_view text = full.substr(0, cut);
        auto it = g_tr.find(text);
        if (it == g_tr.end()) return label;
        // keep the English as the ID, so open sections and the like stay the same in every language
        std::string out = it->second + (cut == std::string_view::npos ? "###" + std::string(full) : std::string(full.substr(cut)));
        if (g_lbl.size() > 20000) return label;   // labels made on the fly (names, numbers): don't let it grow forever
        return g_lbl.emplace(std::string(full), std::move(out)).first->second.c_str();
    }
    std::string TS(const std::string& en) { return T(en.c_str()); }

    std::vector<Option> Options() {
        std::vector<Option> o{{"*", "Game language (automatic)"}, {"", "English"}};
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (const auto& e : std::filesystem::directory_iterator(kDir, ec)) {
            if (!e.is_regular_file(ec) || Lower(e.path().extension().string()) != ".ini") continue;
            const std::string st = Lower(e.path().stem().string());
            if (st == "language" || st.starts_with("_template") || st.ends_with("_updated")) continue;
            files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        for (auto& f : files) o.push_back({f.stem().string(), NameIn(f)});
        return o;
    }
    std::string Current() { return g_choice; }
    std::string LoadedName() { return g_loadedName; }
    int Count() { return static_cast<int>(g_tr.size()); }
    bool Choose(const std::string& file) {
        g_choice = file;
        SaveChoice();
        Load(FileFor(g_choice));
        return g_problem.empty();
    }
    std::string WriteTemplate() {
        std::error_code ec;
        std::filesystem::create_directories(kDir, ec);
        auto write = [&](const std::filesystem::path& p, bool filled) {
            std::ofstream o(p, std::ios::binary | std::ios::trunc);
            o << "; Anatomica menu translation. Save as <Language>.ini in this folder (e.g. German.ini - the same name as the\n"
                 "; game's language setting is picked up automatically), then choose it on the General page.\n"
                 "; Each line of English (en:) gets its translation (tr:) below it. Leave tr: empty to keep the English.\n"
                 "; Keep %s, %d, %.0f, %% and the like exactly as they are, in the same order. \\n = a new line.\n"
                 "; The file is UTF-8. A new or renamed file is only found after a game restart (Mod Organizer only shows the game\n"
                 "; the files that were there when it started); edits to a file the game already loaded show when you pick the\n"
                 "; language again. Menu page names change after a restart.\n"
              << "name: " << (filled ? g_loadedName : std::string("My language")) << "\n\n";
            for (const char* k : kKeys) {
                std::string tr;
                if (filled) if (auto it = g_tr.find(std::string_view(k)); it != g_tr.end()) tr = it->second;
                o << "en: " << Escape(k) << "\n" << "tr: " << Escape(tr) << "\n\n";
            }
            return static_cast<bool>(o);
        };
        const size_t n = std::size(kKeys);
        if (!write(kDir / "_template.ini", false)) return "could not write the template";
        std::string msg = std::format("wrote _template.ini ({} lines)", n);
        if (!g_loaded.empty()) {
            int missing = 0;
            for (const char* k : kKeys) if (!g_tr.count(std::string(k))) ++missing;
            if (write(kDir / (g_loaded + "_updated.ini"), true))
                msg += std::format(" and {}_updated.ini (your translation with {} new or missing lines left empty)", g_loaded, missing);
        }
        return msg + " in Data\\SKSE\\Plugins\\AnatomicaMenu\\Translations";
    }

    void RenderGeneral() {
        if (!Tr::CollapsingHeader("Language")) return;
        static std::vector<Option> opts;
        static double lastScan = -10.0;
        static std::string msg;
        const double now = ImGuiMCP::GetTime();
        if (opts.empty() || now - lastScan > 5.0) { opts = Options(); lastScan = now; }
        int sel = 0;
        for (int i = 0; i < static_cast<int>(opts.size()); ++i) if (opts[i].file == g_choice) sel = i;
        const std::string preview = opts.empty() ? std::string("English") : opts[sel].name;
        if (Tr::BeginCombo("Menu language", preview.c_str())) {
            for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
                if (Tr::Selectable(std::format("{}##lang{}", opts[i].name, i).c_str(), i == sel) && i != sel) {
                    Choose(opts[i].file);
                    msg.clear();
                }
            }
            ImGuiMCP::EndCombo();
        }
        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::BeginTooltip();
            ImGuiMCP::PushTextWrapPos(ImGuiMCP::GetFontSize() * 30.0f);
            Tr::TextUnformatted("Translations are files in Data\\SKSE\\Plugins\\AnatomicaMenu\\Translations. \"Game language\" uses the "
                                "one named like the game's language setting (e.g. German.ini), else English. Picking a language changes the "
                                "menu at once; the page names on the left change after a game restart. A translation file you add or "
                                "rename while the game is running is only found after a restart.");
            ImGuiMCP::PopTextWrapPos();
            ImGuiMCP::EndTooltip();
        }
        Tr::TextDisabled("Added or renamed a translation file? Restart the game first - it is only found then.");
        if (!g_loaded.empty()) Tr::TextDisabled("In use: %s - %d lines translated", g_loadedName.c_str(), Count());
        else if (!g_problem.empty()) Tr::TextColored(ImGuiMCP::ImVec4{1.0f, 0.45f, 0.4f, 1.0f}, "English (%s)", g_problem.c_str());
        else Tr::TextDisabled("In use: English");
        if (g_bad) Tr::TextDisabled("%d lines keep their English where a number is shown: their %%s / %%d do not match the English.", g_bad);
        if (Tr::Button("Write translation template")) msg = WriteTemplate();
        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::BeginTooltip();
            ImGuiMCP::PushTextWrapPos(ImGuiMCP::GetFontSize() * 30.0f);
            Tr::TextUnformatted("Writes _template.ini with every line of the menu in English, ready to translate. With a translation "
                                "in use it also writes <name>_updated.ini: your translation with the lines added since left empty, "
                                "so after an Anatomica update you only fill those in.");
            ImGuiMCP::PopTextWrapPos();
            ImGuiMCP::EndTooltip();
        }
        if (!msg.empty()) Tr::TextWrapped("%s", msg.c_str());
        ImGuiMCP::Spacing();
    }
}
