// tests/test_editor_vocabulary.cpp - owned by Verity / QA. Item 156, the text and contract half.
//
// D42 (ARCHITECTURE.md "Default theme tokens, scale and closed UI vocabulary") froze:
//   * a CLOSED vocabulary - ten canonical labels - held in ONE place (core/Theme.h::UiVocabulary),
//     with the instruction "do not substitute synonyms such as Hierarchy, Inspector, Toolbox, Panel";
//   * a token table (colours, alphas) and a metric table, held in core/Theme.h::ThemeTokens.
// A claim like that rots: a developer types "Outliner" as a literal because it is faster, or writes
// "Tools panel" in a help string, or pastes an ImVec4 colour. This file makes each of those a failing
// test, mechanically, from the SOURCE and from the DOCUMENT:
//
//   1. vocabulary single-source: no string literal in src/ spells a canonical label itself - it must
//      come from UiVocabulary (the only exceptions are named, and printed);
//   2. no banned synonym appears in any user-facing literal;
//   3. UiVocabulary equals the ten labels in the D42 table of ARCHITECTURE.md (so doc and code cannot
//      drift apart);
//   4. the real drawn windows are named from the vocabulary (Outliner / Properties) at run time;
//   5. ThemeTokens equals the D42 token and metric tables (colours, alphas, header 27, widget 19 ...);
//   6. no hard-coded colour literal (IM_COL32 / ImVec4(number...) / ImColor(number...)) in src/editor
//      outside the theme adapter - a colour that bypasses the tokens would defeat the swap test.
// The pixel proof that tokens control the screen is test_editor_theme_swap.cpp.
#include "harness.h"

#include "core/Theme.h"
#include "editor/Panels.h"
#include "editor/Undo.h"
#include "scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

const fs::path kRoot = EMBER_SOURCE_DIR;

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Lit { fs::path file; int line; std::string text; bool diagnostic; };

// One pass over a source file. Returns the CODE with comments and string/char literal CONTENTS
// blanked to spaces (line structure kept), and appends every string literal to `lits`.
std::string scan(const fs::path& file, const std::string& src, std::vector<Lit>& lits) {
    std::string code = src;
    int line = 1;
    const std::size_t n = src.size();
    auto blank = [&](std::size_t i) { if (code[i] != '\n') code[i] = ' '; };
    // Which lines are diagnostics (logging/printf): not user-facing UI.
    std::vector<std::string> lines;
    { std::istringstream ss(src); for (std::string l; std::getline(ss, l);) lines.push_back(l); }
    auto diagLine = [&](int ln) {
        if (ln < 1 || ln > int(lines.size())) return false;
        const std::string& l = lines[std::size_t(ln - 1)];
        return l.find("EMBER_LOG_") != std::string::npos || l.find("printf") != std::string::npos ||
               l.find("checkPie") != std::string::npos || l.find("selftest") != std::string::npos ||
               l.find("ImHashStr") != std::string::npos || l.find("assert") != std::string::npos;
    };
    for (std::size_t i = 0; i < n;) {
        const char c = src[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {            // line comment
            while (i < n && src[i] != '\n') { blank(i); ++i; }
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {            // block comment
            blank(i); blank(i + 1); i += 2;
            while (i < n && !(src[i] == '*' && i + 1 < n && src[i + 1] == '/')) { if (src[i] == '\n') ++line; blank(i); ++i; }
            if (i < n) { blank(i); blank(i + 1); i += 2; }
            continue;
        }
        if (c == '#') {                                              // preprocessor: skip #include "x" lines
            std::size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) ++j;
            if (src.compare(j, 7, "include") == 0) {
                while (i < n && src[i] != '\n') { blank(i); ++i; }
                continue;
            }
        }
        if (c == '\'') {                                             // char literal
            blank(i); ++i;
            while (i < n && src[i] != '\'') { if (src[i] == '\\') { blank(i); ++i; } blank(i); ++i; }
            if (i < n) { blank(i); ++i; }
            continue;
        }
        if (c == '"') {                                              // string literal
            const int startLine = line;
            std::string text;
            ++i;                                                     // keep the quote itself in `code`
            while (i < n && src[i] != '"') {
                if (src[i] == '\n') { ++line; }
                if (src[i] == '\\' && i + 1 < n) { text += src[i + 1]; blank(i); ++i; blank(i); ++i; continue; }
                text += src[i]; blank(i); ++i;
            }
            if (i < n) ++i;
            lits.push_back({file, startLine, text, diagLine(startLine)});
            continue;
        }
        ++i;
    }
    return code;
}

struct Corpus {
    std::vector<Lit> lits;
    std::vector<std::pair<fs::path, std::string>> code;   // comment/literal-blanked
};

const Corpus& corpus() {
    static Corpus c = [] {
        Corpus out;
        for (const auto& e : fs::recursive_directory_iterator(kRoot / "src")) {
            if (!e.is_regular_file()) continue;
            const std::string ext = e.path().extension().string();
            if (ext != ".cpp" && ext != ".h") continue;
            const std::string s = slurp(e.path());
            out.code.emplace_back(e.path(), scan(e.path(), s, out.lits));
        }
        return out;
    }();
    return c;
}

std::string rel(const fs::path& p) { return fs::relative(p, kRoot).generic_string(); }

bool isVocabFile(const fs::path& p) {
    const std::string r = rel(p);
    return r == "src/core/Theme.h";
}
bool isInternalId(const Lit& l, const std::string& r) {
    // ImGui IDs and the documented stable docking IDs are not user-facing text.
    return l.text.rfind("##", 0) == 0 || r == "src/editor/EditorLabels.h";
}

const char* kVocab[] = {"Outliner", "Properties", "Sidebar", "Toolbar", "Area", "Header", "Region", "Collection", "Modifier", "Object Mode"};

} // namespace

// ---- 1. the closed vocabulary lives in ONE place -----------------------------------------------

// Two strengths of the same rule. (a) A literal that IS a canonical label is a pure second source.
// (b) A canonical label typed inside a longer literal ('New Collection', 'Toggle Sidebar') still
// freezes the old spelling in place if UiVocabulary ever changes: it should be composed from it,
// as Outliner.cpp already does for its own 'New Collection' menu item.
static void vocabularyScan(bool exactOnly) {
    std::string words;
    for (const char* w : kVocab) words += (words.empty() ? "" : "|") + std::string(w);
    const std::regex embedded("\\b(" + words + ")s?\\b");
    const std::regex exact("^(" + words + ")s?$");
    int checked = 0, found = 0;
    for (const Lit& l : corpus().lits) {
        const std::string r = rel(l.file);
        if (isVocabFile(l.file) || isInternalId(l, r) || l.diagnostic) continue;
        ++checked;
        std::smatch m;
        const bool isExact = std::regex_match(l.text, exact);
        const bool hit = exactOnly ? isExact : (!isExact && std::regex_search(l.text, m, embedded));
        if (hit) {
            ++found;
            const std::string w = exactOnly ? l.text : m[1].str();
            CHECK_MSG(false, "%s:%d: the literal \"%s\" %s the canonical label '%s' - use ember::UiVocabulary so there is ONE source",
                      r.c_str(), l.line, l.text.c_str(), exactOnly ? "IS" : "embeds", w.c_str());
        } else {
            ++harness::g_checks;
        }
    }
    std::printf("       scanned %d user-facing string literals under src/; %s canonical labels: %d\n", checked,
                exactOnly ? "literals that ARE" : "literals that embed", found);
    CHECK_MSG(checked > 200, "only %d literals scanned - the scanner is not seeing the source tree", checked);
}

TEST(vocabulary_no_literal_IS_a_canonical_label) { vocabularyScan(true); }

TEST(vocabulary_no_literal_embeds_a_canonical_label) { vocabularyScan(false); }

TEST(vocabulary_no_banned_synonym_in_any_user_facing_literal) {
    // ARCHITECTURE.md D42: 'Do not substitute synonyms such as Hierarchy, Inspector, Toolbox, Panel'.
    const std::regex banned("\\b(hierarchy|inspector|toolbox|panels?)\\b", std::regex::icase);
    int hits = 0;
    for (const Lit& l : corpus().lits) {
        if (l.diagnostic || l.text.rfind("##", 0) == 0) continue;
        std::smatch m;
        if (std::regex_search(l.text, m, banned)) {
            ++hits;
            CHECK_MSG(false, "%s:%d: banned synonym '%s' in the user-facing text \"%s\" (the vocabulary word is Toolbar / Properties / Outliner / Region / Area ...)",
                      rel(l.file).c_str(), l.line, m[1].str().c_str(), l.text.c_str());
        } else {
            ++harness::g_checks;
        }
    }
    std::printf("       banned-synonym hits: %d\n", hits);
}

TEST(vocabulary_UiVocabulary_equals_the_D42_table_in_ARCHITECTURE_md) {
    const std::string doc = slurp(kRoot / "ARCHITECTURE.md");
    const std::size_t at = doc.find("Closed user-facing vocabulary");
    CHECK_MSG(at != std::string::npos, "ARCHITECTURE.md no longer has the 'Closed user-facing vocabulary' table");
    if (at == std::string::npos) return;
    std::set<std::string> fromDoc;
    std::istringstream ss(doc.substr(at, 2500));
    const std::regex row("^\\|\\s*[^|]+\\|\\s*`([^`]+)`\\s*\\|");
    for (std::string line; std::getline(ss, line);) {
        std::smatch m;
        if (std::regex_search(line, m, row)) fromDoc.insert(m[1].str());
    }
    const std::set<std::string> fromCode = {ember::UiVocabulary::Outliner, ember::UiVocabulary::Properties, ember::UiVocabulary::Sidebar,
        ember::UiVocabulary::Toolbar, ember::UiVocabulary::Area, ember::UiVocabulary::Header, ember::UiVocabulary::Region,
        ember::UiVocabulary::Collection, ember::UiVocabulary::Modifier, ember::UiVocabulary::ObjectMode};
    CHECK_MSG(fromDoc.size() == 10, "the D42 table lists %zu labels, expected the ten canonical ones", fromDoc.size());
    CHECK_MSG(fromCode.size() == 10, "UiVocabulary holds %zu distinct labels, expected 10", fromCode.size());
    for (const std::string& w : fromDoc) CHECK_MSG(fromCode.count(w) == 1, "the doc says the label is '%s' but UiVocabulary has no such string", w.c_str());
    for (const std::string& w : fromCode) CHECK_MSG(fromDoc.count(w) == 1, "UiVocabulary has '%s' which the D42 table does not list", w.c_str());
}

TEST(vocabulary_drawn_windows_are_named_from_the_vocabulary_at_run_time) {
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* px = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    ember::scene::World world;
    ember::editor::UndoStack undo;
    world.createPrimitive(ember::scene::Primitive::Cube, "cube");
    for (int f = 0; f < 3; ++f) {
        ImGui::NewFrame();
        ember::editor::drawOutliner(world, undo);
        ember::editor::drawProperties(world, undo);
        ImGui::EndFrame();
    }
    std::set<std::string> names;
    for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows) names.insert(win->Name);
    CHECK_MSG(names.count(ember::UiVocabulary::Outliner) == 1, "no window named '%s' was drawn by drawOutliner", ember::UiVocabulary::Outliner);
    CHECK_MSG(names.count(ember::UiVocabulary::Properties) == 1, "no window named '%s' was drawn by drawProperties", ember::UiVocabulary::Properties);
    const std::regex banned("\\b(hierarchy|inspector|toolbox|panels?)\\b", std::regex::icase);
    for (const std::string& n : names) CHECK_MSG(!std::regex_search(n, banned), "a drawn window is named '%s': banned synonym", n.c_str());
    ImGui::DestroyContext(ctx);
}

// ---- 2. the tokens equal the frozen D42 tables --------------------------------------------------

namespace {
struct Rgba { int r, g, b, a; };
bool parseHex(const std::string& s, Rgba& out) {   // #RRGGBB or #RRGGBBAA
    if (s.size() != 7 && s.size() != 9) return false;
    auto h = [&](std::size_t i) { return std::stoi(s.substr(i, 2), nullptr, 16); };
    out = {h(1), h(3), h(5), s.size() == 9 ? h(7) : 255};
    return true;
}
} // namespace

TEST(tokens_ThemeTokens_equals_the_D42_colour_table) {
    const std::string doc = slurp(kRoot / "ARCHITECTURE.md");
    const std::size_t at = doc.find("| `header.bg`");
    CHECK_MSG(at != std::string::npos, "the D42 token table ('header.bg' row) is gone from ARCHITECTURE.md");
    if (at == std::string::npos) return;
    const ember::ThemeTokens& t = ember::kDefaultTheme;
    auto colour = [&](const char* tok, const ember::ThemeColor& c, bool ignoreAlpha) {
        const std::regex row(std::string("\\|\\s*`") + tok + "`\\s*\\|\\s*`(#[0-9A-Fa-f]+)`");
        std::smatch m;
        if (!std::regex_search(doc, m, row)) { CHECK_MSG(false, "no '%s' row in the D42 token table", tok); return; }
        Rgba want{};
        CHECK_MSG(parseHex(m[1].str(), want), "token '%s': cannot parse %s", tok, m[1].str().c_str());
        CHECK_MSG(c.r == want.r && c.g == want.g && c.b == want.b && (ignoreAlpha || c.a == want.a),
                  "token '%s': Theme.h has (%d,%d,%d,%d), the D42 table says %s = (%d,%d,%d,%d)", tok, c.r, c.g, c.b, c.a, m[1].str().c_str(),
                  want.r, want.g, want.b, want.a);
        if (ignoreAlpha) {   // the doc folds the region alpha into the hex; it must equal the separate float token
            const double a = t.regionBackgroundAlpha * 255.0;
            CHECK_MSG(std::fabs(a - want.a) <= 1.0, "region alpha: Theme.h %.3f*255=%.1f, the doc's #...%02X is %d", t.regionBackgroundAlpha, a, want.a, want.a);
        }
    };
    colour("header.bg", t.headerBackground, false);
    colour("region.bg", t.regionBackground, true);
    colour("widget.field.bg", t.widgetFieldBackground, false);
    colour("widget.button.bg", t.widgetButtonBackground, false);
    colour("text.primary", t.textPrimary, false);
    colour("text.disabled", t.textDisabled, false);
    colour("selection", t.selection, false);
    colour("active", t.active, false);
    colour("hover", t.hover, false);
    CHECK_MSG(t.viewportBodyBackground.a == 0 && t.viewportBodyAlpha == 0.0f, "viewport.body.bg must be alpha 0 (the scene shows through the host)");
    // The two states must stay semantically distinct (D42): selection orange, active blue, hover neutral.
    CHECK_MSG(t.selection.r > t.selection.b + 100, "selection must stay orange (R >> B): (%d,%d,%d)", t.selection.r, t.selection.g, t.selection.b);
    CHECK_MSG(t.active.b > t.active.r + 50, "active must stay blue (B >> R): (%d,%d,%d)", t.active.r, t.active.g, t.active.b);
    CHECK_MSG(t.hover.r == t.hover.g && t.hover.g == t.hover.b, "hover must stay a neutral wash (R==G==B): (%d,%d,%d)", t.hover.r, t.hover.g, t.hover.b);
}

TEST(tokens_ThemeTokens_equals_the_D42_metric_table) {
    const ember::ThemeTokens& t = ember::kDefaultTheme;
    const std::string doc = slurp(kRoot / "ARCHITECTURE.md");
    CHECK_MSG(doc.find("| Area Header height | `27` logical px |") != std::string::npos, "the D42 metric table no longer says Header height 27 (update this test with the doc on purpose)");
    CHECK_MSG(doc.find("`19` logical px") != std::string::npos, "the D42 metric table no longer says widget height 19");
    CHECK_MSG(t.headerHeight == 27.0f, "headerHeight %g, D42 says 27", double(t.headerHeight));
    CHECK_MSG(t.widgetHeight == 19.0f, "widgetHeight %g, D42 says 19", double(t.widgetHeight));
    CHECK_MSG(t.itemSpacing.x == 8.0f && t.itemSpacing.y == 4.0f, "itemSpacing (%g,%g), D42 says 8x4", double(t.itemSpacing.x), double(t.itemSpacing.y));
    CHECK_MSG(t.itemInnerSpacing.x == 4.0f && t.itemInnerSpacing.y == 4.0f, "itemInnerSpacing (%g,%g), D42 says 4x4", double(t.itemInnerSpacing.x), double(t.itemInnerSpacing.y));
    CHECK_MSG(t.regionRounding == 0.0f, "regionRounding %g, D42 says 0 (square tiled areas)", double(t.regionRounding));
    CHECK_MSG(t.widgetRounding == 3.0f, "widgetRounding %g, D42 says 3", double(t.widgetRounding));
    CHECK_MSG(std::fabs(t.regionBackgroundAlpha - 0.96f) < 1e-6f, "regionBackgroundAlpha %g, D42 says 0.96", double(t.regionBackgroundAlpha));
    CHECK_MSG(std::fabs(t.selectionFillAlpha - 0.45f) < 1e-6f, "selection fill alpha %g, D42 says 45%%", double(t.selectionFillAlpha));
    CHECK_MSG(std::fabs(t.activeFillAlpha - 0.85f) < 1e-6f, "active fill alpha %g, D42 says 85%%", double(t.activeFillAlpha));
    CHECK_MSG(t.uiScale == 1.0f, "uiScale %g, the design baseline is 1.0", double(t.uiScale));
}

// ---- 3. nothing paints a colour from a literal --------------------------------------------------

TEST(theme_no_hard_coded_colour_literal_in_the_editor) {
    // A colour typed into a widget call bypasses the tokens: editing the token then changes nothing
    // on screen. Flag IM_COL32(...), ImColor(<number>...), ImVec4(<number>...).
    const std::regex lit("(IM_COL32\\s*\\()|(ImColor\\s*\\(\\s*-?[0-9.])|(ImVec4\\s*\\(\\s*-?[0-9.])");
    int hits = 0, files = 0;
    for (const auto& [path, code] : corpus().code) {
        const std::string r = rel(path);
        if (r.rfind("src/editor/", 0) != 0) continue;
        ++files;
        int line = 1;
        std::istringstream ss(code);
        for (std::string l; std::getline(ss, l); ++line) {
            if (std::regex_search(l, lit)) {
                ++hits;
                CHECK_MSG(false, "%s:%d: hard-coded colour literal '%s' - colours come from the theme tokens (a token edit would not change this pixel)",
                          r.c_str(), line, l.c_str());
            }
        }
    }
    std::printf("       scanned %d editor files; hard-coded colour literals: %d\n", files, hits);
    CHECK_MSG(files >= 15, "only %d editor files scanned", files);
    ++harness::g_checks;
}

// ---- 4. no menu item may be ENABLED AND INERT (item 202; the class behind item 199) --------------
//
// A MenuItem that is drawn, enabled and clickable but has no handler lies to the user: item 199 was
// exactly that for the Render items. This is a source scan, like the colour-literal rule above. The
// accepted forms - the idiom of this tree - are:
//   1. a handler:       if (ImGui::MenuItem("Rename")) beginRename(...);      (non-empty body)
//   2. the result used: const bool pressed = ImGui::MenuItem(...);            (assigned, or returned)
//   3. disabled:        ImGui::MenuItem("... (not available yet)", nullptr, false, false);
//   4. a state toggle:  ImGui::MenuItem("Sidebar", "N", &sidebar);            (pointer-selected writes state)
//   5. inside an ImGui::BeginDisabled() ... EndDisabled() scope.
// Anything else is enabled and inert and FAILS. A PERMANENTLY unavailable item (a literal `false` enabled
// argument, or an unconditional BeginDisabled() scope) whose LABEL states no reason is printed as a
// warning, not a failure: it is not the defect class, but it is not the idiomatic form either - the Render
// items state a reason in the label. A conditional disable ("nothing is selected") needs no reason, so it
// is not warned about.
namespace {

struct MenuSite {
    fs::path file;
    int line = 0;
    std::string call;                 // the comment/literal-blanked call, trimmed
    std::string label;                // the label literal on that line, if any (for the message)
    bool inert = false;               // enabled, no handler, no disabled flag
    bool disabled = false;
    bool permanentDisable = false;    // inside an unconditional BeginDisabled() scope
    bool needsReason = false;         // permanently unavailable, so the label should say why
    bool statesReason = false;        // the label explains why it cannot be used
    bool handler = false, assigned = false, toggle = false, inDisabledScope = false;
};

std::vector<std::string> splitArgs(const std::string& inside) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    bool inStr = false;
    for (const char c : inside) {
        if (c == '"') inStr = !inStr;
        if (!inStr) {
            if (c == '(' || c == '[' || c == '{') ++depth;
            if (c == ')' || c == ']' || c == '}') --depth;
            if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); continue; }
        }
        cur += c;
    }
    if (!cur.empty() || !out.empty()) out.push_back(cur);
    return out;
}

std::string trimmed(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// "Does this statement do anything?" - whitespace, braces and semicolons are not an action.
bool doesSomething(const std::string& body) {
    for (const char c : body) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ';' || c == '{' || c == '}') continue;
        return true;
    }
    return false;
}

// Classify every MenuItem call in one blob of (comment/literal-blanked) code.
void menuSitesIn(const std::string& code, const fs::path& file,
                 const std::map<int, std::string>& labelByLine, std::vector<MenuSite>& out) {
    const std::string needle = "MenuItem(";
    // BeginDisabled(cond) is conditional; BeginDisabled() is permanent. Only the permanent form needs a
    // reason in the label - "disabled because nothing is selected" explains itself.
    struct Scope { std::size_t at; bool open; bool unconditional; };
    std::vector<Scope> scopes;
    for (std::size_t p = code.find("BeginDisabled("); p != std::string::npos; p = code.find("BeginDisabled(", p + 1)) {
        const std::size_t open = p + std::strlen("BeginDisabled");
        const std::size_t close = code.find(')', open);
        const std::string arg = close == std::string::npos ? std::string() : trimmed(code.substr(open + 1, close - open - 1));
        scopes.push_back({p, true, arg.empty()});
    }
    for (std::size_t p = code.find("EndDisabled("); p != std::string::npos; p = code.find("EndDisabled(", p + 1))
        scopes.push_back({p, false, false});
    std::sort(scopes.begin(), scopes.end(), [](const Scope& a, const Scope& b) { return a.at < b.at; });
    std::size_t scopeIdx = 0;
    std::vector<Scope> openScopes;

    for (std::size_t at = code.find(needle); at != std::string::npos; at = code.find(needle, at + 1)) {
        while (scopeIdx < scopes.size() && scopes[scopeIdx].at < at) {
            const Scope& sc = scopes[scopeIdx++];
            if (sc.open) openScopes.push_back(sc);
            else if (!openScopes.empty()) openScopes.pop_back();
        }
        // Balanced call: from the '(' of the call to its matching ')'.
        const std::size_t open = at + needle.size() - 1;
        int depth = 0;
        std::size_t close = std::string::npos;
        bool inStr = false;
        for (std::size_t i = open; i < code.size(); ++i) {
            const char c = code[i];
            if (c == '"') inStr = !inStr;
            if (inStr) continue;
            if (c == '(') ++depth;
            else if (c == ')') { if (--depth == 0) { close = i; break; } }
        }
        if (close == std::string::npos) continue;

        MenuSite s;
        s.file = file;
        s.line = 1 + int(std::count(code.begin(), code.begin() + std::ptrdiff_t(at), '\n'));
        s.call = trimmed(code.substr(at, close - at + 1));
        // The label only matters for the reason check: it is the literal inside THIS call.
        if (s.call.find('"') != std::string::npos) {
            const auto it = labelByLine.find(s.line);
            if (it != labelByLine.end()) s.label = it->second;
        }
        s.statesReason = !s.label.empty() && s.label.find('(') != std::string::npos;

        const std::vector<std::string> args = splitArgs(code.substr(open + 1, close - open - 1));
        bool literalFalse = false;
        if (args.size() >= 4) {
            literalFalse = trimmed(args[3]) == "false";
            s.disabled = trimmed(args[3]) != "true";                 // form 3
        } else if (args.size() == 3 && args[2].find('&') != std::string::npos) {
            s.toggle = true;                                         // form 4
        }
        s.inDisabledScope = !openScopes.empty();                      // form 5
        for (const Scope& sc : openScopes) s.permanentDisable = s.permanentDisable || sc.unconditional;
        s.disabled = s.disabled || s.inDisabledScope;
        s.needsReason = literalFalse || s.permanentDisable;

        // The statement the call sits in: walk back to the previous ; { } or a blank line.
        std::size_t stmt = at;
        int linesBack = 0;
        while (stmt > 0) {
            const char c = code[stmt - 1];
            if (c == ';' || c == '}' || c == '{' || c == '\n') {
                if (c == '\n') { ++linesBack; if (linesBack > 2) break; }
                else break;
            }
            --stmt;
        }
        const std::string prefix = trimmed(code.substr(stmt, at - stmt));
        // The call is written as `ImGui::MenuItem(...)`, so the prefix ends with a qualified name.
        const bool condition = std::regex_search(prefix, std::regex("if\\s*\\(\\s*!?\\s*(\\w+::)*$"));
        const bool returned = std::regex_search(prefix, std::regex("return\\s+(\\w+::)*$"));
        const bool assign = prefix.find('=') != std::string::npos && prefix.find("==") == std::string::npos &&
                            prefix.find("!=") == std::string::npos && prefix.find("<=") == std::string::npos &&
                            prefix.find(">=") == std::string::npos;
        if (returned || assign) s.assigned = true;                                     // form 2
        if (condition) {
            std::size_t body = close + 1;
            while (body < code.size() && std::isspace(static_cast<unsigned char>(code[body]))) ++body;
            std::string stmtBody;
            if (body < code.size() && code[body] == '{') {
                std::size_t b2 = body;
                int d = 0;
                for (; b2 < code.size(); ++b2) {
                    if (code[b2] == '{') ++d;
                    else if (code[b2] == '}') { if (--d == 0) { ++b2; break; } }
                }
                stmtBody = code.substr(body, b2 - body);
            } else {
                const std::size_t semi = code.find(';', body);
                stmtBody = code.substr(body, (semi == std::string::npos ? code.size() : semi) - body);
            }
            s.handler = doesSomething(stmtBody);                                       // form 1
        }
        s.inert = !s.handler && !s.assigned && !s.disabled && !s.toggle;
        out.push_back(s);
    }
}

std::vector<MenuSite> menuSitesInEditor() {
    std::vector<MenuSite> out;
    for (const auto& [path, code] : corpus().code) {
        const std::string r = rel(path);
        if (r.rfind("src/editor/", 0) != 0) continue;
        std::map<int, std::string> labels;
        for (const Lit& l : corpus().lits) if (l.file == path) labels.emplace(l.line, l.text);
        menuSitesIn(code, path, labels, out);
    }
    return out;
}

} // namespace

TEST(no_menu_item_is_enabled_and_inert) {
    // CONTROL FIRST: the predicate must fail on the bare form, and must accept all five idioms.
    {
        const std::map<int, std::string> none;
        const std::string synthetic =
            "void f() {\n"
            "  if (ImGui::MenuItem(\"A\")) act();\n"                       // 1 handler
            "  const bool pressed = ImGui::MenuItem(\"B\");\n"              // 2 result used
            "  ImGui::MenuItem(\"C (not available yet)\", nullptr, false, false);\n"  // 3 disabled
            "  ImGui::MenuItem(\"Sidebar\", \"N\", &sidebar);\n"            // 4 toggle
            "  ImGui::BeginDisabled();\n"
            "  ImGui::MenuItem(\"E\");\n"                                   // 5 disabled scope
            "  ImGui::EndDisabled();\n"
            "  ImGui::MenuItem(\"F\");\n"                                   // <-- ENABLED + INERT
            "}";
        std::vector<MenuSite> syntheticSites;
        menuSitesIn(synthetic, fs::path("synthetic.cpp"), none, syntheticSites);
        CHECK_MSG(syntheticSites.size() == 6, "control setup: the classifier saw %zu of 6 synthetic calls",
                  syntheticSites.size());
        int syntheticInert = 0;
        for (const MenuSite& s : syntheticSites) if (s.inert) ++syntheticInert;
        CHECK_MSG(syntheticInert == 1,
                  "control: the rule reported %d inert item(s) in a snippet with exactly one bare "
                  "ImGui::MenuItem - it either misses the defect or flags an accepted idiom",
                  syntheticInert);
    }

    const std::vector<MenuSite> sites = menuSitesInEditor();
    CHECK_MSG(sites.size() >= 20, "only %zu ImGui::MenuItem call sites found under src/editor - the scan "
              "is not seeing the sources", sites.size());
    int inert = 0, disabled = 0, reasonless = 0, handlers = 0;
    for (const MenuSite& s : sites) {
        if (s.inert) {
            ++inert;
            CHECK_MSG(false, "%s:%d: menu item %s is ENABLED AND INERT - no handler and no disabled flag. Either "
                      "wire the action, or use the disabled form with a reason (e.g. "
                      "MenuItem(\"<label> (not available yet)\", nullptr, false, false))",
                      rel(s.file).c_str(), s.line, s.call.c_str());
        }
        if (s.disabled) { ++disabled; if (s.needsReason && !s.statesReason) ++reasonless; }
        if (s.handler || s.assigned) ++handlers;
    }
    std::printf("       scanned %zu ImGui::MenuItem call sites under src/editor: %d with a handler (or with "
                "the result used), %d disabled, %d inert\n", sites.size(), handlers, disabled, inert);
    if (reasonless > 0) {
        std::printf("       WARNING (not a failure): %d PERMANENTLY unavailable item(s) state no reason in the "
                    "label (a conditional disable - 'nothing is selected' - needs none; the idiomatic "
                    "permanent form carries one, like the Render items do):\n", reasonless);
        for (const MenuSite& s : sites)
            if (s.needsReason && !s.statesReason)
                std::printf("         %s:%d  %s\n", rel(s.file).c_str(), s.line,
                            s.label.empty() ? s.call.c_str() : s.label.c_str());
    }
    CHECK_MSG(inert == 0, "%d menu item(s) are enabled and inert", inert);
}

HARNESS_MAIN()
