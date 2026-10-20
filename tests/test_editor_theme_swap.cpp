// tests/test_editor_theme_swap.cpp - owned by Verity / QA. Item 156, the pixel half.
//
// Claim under test: a theme TOKEN actually controls what is DRAWN. A theme nobody tests drifts back
// to hard-coded literals: someone writes IM_COL32(34,34,34,255) next to the token that says #222222
// and nothing notices until the day someone edits the token and the screen does not change.
//
// METHOD: tests/CMakeLists.txt compiles the REAL editor sources twice (see theme_probe.cpp):
//   theme_probe_default  against the real src/core/Theme.h
//   theme_probe_swapped  against a generated Theme.h with every colour/alpha/metric token CHANGED
// Each probe runs the real applyTheme() and real drawArea(), draws real widgets, software-rasterises
// the actual ImDrawData over a known backdrop and prints the pixels. THIS test then
//   1. computes, for each element, the pixel the TOKENS predict (source-over of the token values
//      over the backdrop - no editor code involved),
//   2. asserts the DEFAULT build drew exactly that,
//   3. asserts the SWAPPED build drew exactly that for the swapped values,
//   4. asserts the two builds differ on every element (so nothing is drawn from a literal that
//      bypasses its token: that element would be identical in both builds and fail here).
// Elements: Header strip, Region (area) background, 3D Viewport body (transparent), selected row,
// button at rest / hovered / pressed, input field, primary and disabled text, plus the Header
// height and the input-field height metrics.
#include "harness.h"

#include "core/Theme.h"
#include "swap_values.h"   // GENERATED from tests/theme_swap_values.cmake: the swapped token values

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

struct V { std::vector<double> v; };
using Measure = std::map<std::string, V>;

struct Theme {
    double header[3], region[3], viewport[3], field[3], button[3], selection[3], active[3];
    double text[4], textDisabled[4], hover[4];
    double regionAlpha, viewportAlpha, selectionFillAlpha, activeFillAlpha, headerHeight, widgetHeight;
};

template <typename C> void rgb(double out[3], const C& c) { out[0] = c.r; out[1] = c.g; out[2] = c.b; }

Theme fromDefaults() {
    const ember::ThemeTokens& t = ember::kDefaultTheme;
    Theme o{};
    rgb(o.header, t.headerBackground); rgb(o.region, t.regionBackground); rgb(o.viewport, t.viewportBodyBackground);
    rgb(o.field, t.widgetFieldBackground); rgb(o.button, t.widgetButtonBackground);
    rgb(o.selection, t.selection); rgb(o.active, t.active);
    const ember::ThemeColor* tc[3] = {&t.textPrimary, &t.textDisabled, &t.hover};
    double* dst[3] = {o.text, o.textDisabled, o.hover};
    for (int i = 0; i < 3; ++i) { dst[i][0] = tc[i]->r; dst[i][1] = tc[i]->g; dst[i][2] = tc[i]->b; dst[i][3] = tc[i]->a; }
    o.regionAlpha = t.regionBackgroundAlpha; o.viewportAlpha = t.viewportBodyAlpha;
    o.selectionFillAlpha = t.selectionFillAlpha; o.activeFillAlpha = t.activeFillAlpha;
    o.headerHeight = t.headerHeight; o.widgetHeight = t.widgetHeight;
    return o;
}

Theme fromSwap() {
    Theme o{};
    for (int i = 0; i < 3; ++i) {
        o.header[i] = swap::headerBackground[i]; o.region[i] = swap::regionBackground[i];
        o.viewport[i] = swap::viewportBodyBackground[i]; o.field[i] = swap::widgetFieldBackground[i];
        o.button[i] = swap::widgetButtonBackground[i]; o.selection[i] = swap::selection[i]; o.active[i] = swap::active[i];
    }
    for (int i = 0; i < 4; ++i) { o.text[i] = swap::textPrimary[i]; o.textDisabled[i] = swap::textDisabled[i]; o.hover[i] = swap::hover[i]; }
    o.regionAlpha = swap::regionBackgroundAlpha; o.viewportAlpha = swap::viewportBodyAlpha;
    o.selectionFillAlpha = swap::selectionFillAlpha; o.activeFillAlpha = swap::activeFillAlpha;
    o.headerHeight = swap::headerHeight; o.widgetHeight = swap::widgetHeight;
    return o;
}

const double K[3] = {200.0, 100.0, 50.0};   // the probes' backdrop

struct P3 { double c[3]; };
P3 blend(const double dst[3], const double src[3], double a) {
    return {{dst[0] * (1 - a) + src[0] * a, dst[1] * (1 - a) + src[1] * a, dst[2] * (1 - a) + src[2] * a}};
}

// What the tokens say each probed pixel must be.
std::map<std::string, P3> expected(const Theme& t) {
    std::map<std::string, P3> e;
    const P3 W = blend(K, t.region, t.regionAlpha);                      // area / window background over the backdrop
    e["region"] = W;
    e["header"] = {{t.header[0], t.header[1], t.header[2]}};             // opaque strip
    e["viewport_body"] = blend(K, t.viewport, t.viewportAlpha);          // transparent host (alpha 0 by default)
    e["selected_row"] = blend(W.c, t.selection, t.selectionFillAlpha);
    e["button"] = {{t.button[0], t.button[1], t.button[2]}};
    e["button_hover"] = blend(W.c, t.hover, t.hover[3] / 255.0);
    e["button_active"] = blend(W.c, t.active, t.activeFillAlpha);
    e["field"] = {{t.field[0], t.field[1], t.field[2]}};
    return e;
}

bool run(const char* exeEnv, Measure& out, std::string& why) {
    const char* exe = std::getenv(exeEnv);
    if (!exe || !*exe) { why = std::string(exeEnv) + " is not set (CTest sets it to the probe target)"; return false; }
    const std::string outFile = std::string(exe) + ".out.txt";
    const std::string cmd = std::string("\"\"") + exe + "\" > \"" + outFile + "\" 2>&1\"";
    const int rc = std::system(cmd.c_str());
    if (rc != 0) { why = std::string(exe) + " exited " + std::to_string(rc); return false; }
    std::ifstream f(outFile);
    for (std::string line; std::getline(f, line);) {
        std::istringstream ss(line);
        std::string key;
        if (!(ss >> key)) continue;
        V v; double x;
        while (ss >> x) v.v.push_back(x);
        if (!v.v.empty()) out[key] = v;
    }
    return true;
}

double maxDelta(const V& got, const P3& want) {
    double m = 0;
    for (int i = 0; i < 3; ++i) m = std::fmax(m, std::fabs(got.v[std::size_t(i)] - want.c[i]));
    return m;
}
double sumDelta(const V& a, const V& b) {
    double s = 0;
    for (std::size_t i = 0; i < 3 && i < a.v.size() && i < b.v.size(); ++i) s += std::fabs(a.v[i] - b.v[i]);
    return s;
}
std::string str(const V& v) {
    std::string s = "(";
    for (std::size_t i = 0; i < v.v.size(); ++i) s += (i ? "," : "") + std::to_string(int(std::lround(v.v[i])));
    return s + ")";
}
std::string str(const P3& p) { return "(" + std::to_string(int(std::lround(p.c[0]))) + "," + std::to_string(int(std::lround(p.c[1]))) + "," + std::to_string(int(std::lround(p.c[2]))) + ")"; }

const char* kPixelKeys[] = {"header", "region", "viewport_body", "selected_row", "button", "button_hover", "button_active", "field"};

} // namespace

TEST(theme_swapped_values_are_really_different_from_the_defaults) {
    // Setup guard: if a swap value ever equals its default the whole test would pass vacuously.
    const Theme a = fromDefaults(), b = fromSwap();
    auto differs = [](const double* x, const double* y, int n) { for (int i = 0; i < n; ++i) if (std::fabs(x[i] - y[i]) > 20.0) return true; return false; };
    CHECK_MSG(differs(a.header, b.header, 3), "header token not swapped");
    CHECK_MSG(differs(a.region, b.region, 3), "region token not swapped");
    CHECK_MSG(differs(a.viewport, b.viewport, 3), "viewport body token not swapped");
    CHECK_MSG(differs(a.field, b.field, 3), "field token not swapped");
    CHECK_MSG(differs(a.button, b.button, 3), "button token not swapped");
    CHECK_MSG(differs(a.selection, b.selection, 3), "selection token not swapped");
    CHECK_MSG(differs(a.active, b.active, 3), "active token not swapped");
    CHECK_MSG(differs(a.text, b.text, 3), "text token not swapped");
    CHECK_MSG(differs(a.textDisabled, b.textDisabled, 3), "disabled text token not swapped");
    CHECK_MSG(differs(a.hover, b.hover, 4), "hover token not swapped");
    CHECK_MSG(std::fabs(a.regionAlpha - b.regionAlpha) > 0.2, "region alpha not swapped");
    CHECK_MSG(std::fabs(a.headerHeight - b.headerHeight) > 5.0, "header height not swapped");
    CHECK_MSG(std::fabs(a.widgetHeight - b.widgetHeight) > 5.0, "widget height not swapped");
}

TEST(default_build_draws_exactly_what_the_default_tokens_say) {
    Measure m; std::string why;
    CHECK_MSG(run("EMBER_PROBE_DEFAULT", m, why), "%s", why.c_str());
    if (m.empty()) return;
    const Theme t = fromDefaults();
    const auto e = expected(t);
    for (const char* k : kPixelKeys) {
        CHECK_MSG(m.count(k) > 0, "probe printed no '%s' measurement", k);
        if (!m.count(k)) continue;
        const double d = maxDelta(m[k], e.at(k));
        std::printf("       default %-14s drew %s  tokens predict %s\n", k, str(m[k]).c_str(), str(e.at(k)).c_str());
        CHECK_MSG(d <= 3.0, "DEFAULT '%s' drew %s but the tokens predict %s (max channel error %.1f): something is drawn from a literal, not the token",
                  k, str(m[k]).c_str(), str(e.at(k)).c_str(), d);
    }
    for (int i = 0; i < 2; ++i) {
        const char* k = i == 0 ? "text_primary" : "text_disabled";
        const double* want = i == 0 ? t.text : t.textDisabled;
        CHECK_MSG(m.count(k) > 0 && m[k].v.size() >= 4, "probe printed no '%s'", k);
        if (m.count(k) && m[k].v.size() >= 4)
            CHECK_MSG(std::fabs(m[k].v[0] - want[0]) <= 1 && std::fabs(m[k].v[1] - want[1]) <= 1 && std::fabs(m[k].v[2] - want[2]) <= 1,
                      "DEFAULT %s drew %s, token says (%g,%g,%g)", k, str(m[k]).c_str(), want[0], want[1], want[2]);
    }
    CHECK_MSG(m.count("header_height") && std::fabs(m["header_height"].v[0] - t.headerHeight) <= 1.0, "DEFAULT header height %s, token says %g",
              m.count("header_height") ? str(m["header_height"]).c_str() : "(none)", t.headerHeight);
    CHECK_MSG(m.count("field_height") && std::fabs(m["field_height"].v[0] - t.widgetHeight) <= 1.0, "DEFAULT input-field height %s, token says %g",
              m.count("field_height") ? str(m["field_height"]).c_str() : "(none)", t.widgetHeight);
}

TEST(swapped_build_draws_exactly_what_the_swapped_tokens_say) {
    Measure m; std::string why;
    CHECK_MSG(run("EMBER_PROBE_SWAPPED", m, why), "%s", why.c_str());
    if (m.empty()) return;
    const Theme t = fromSwap();
    const auto e = expected(t);
    for (const char* k : kPixelKeys) {
        CHECK_MSG(m.count(k) > 0, "probe printed no '%s' measurement", k);
        if (!m.count(k)) continue;
        const double d = maxDelta(m[k], e.at(k));
        std::printf("       swapped %-14s drew %s  tokens predict %s\n", k, str(m[k]).c_str(), str(e.at(k)).c_str());
        CHECK_MSG(d <= 3.0, "after the swap '%s' drew %s but the tokens now predict %s (max channel error %.1f): the token does not control this pixel",
                  k, str(m[k]).c_str(), str(e.at(k)).c_str(), d);
    }
    for (int i = 0; i < 2; ++i) {
        const char* k = i == 0 ? "text_primary" : "text_disabled";
        const double* want = i == 0 ? t.text : t.textDisabled;
        CHECK_MSG(m.count(k) > 0 && m[k].v.size() >= 4, "probe printed no '%s'", k);
        if (m.count(k) && m[k].v.size() >= 4)
            CHECK_MSG(std::fabs(m[k].v[0] - want[0]) <= 1 && std::fabs(m[k].v[1] - want[1]) <= 1 && std::fabs(m[k].v[2] - want[2]) <= 1,
                      "after the swap %s drew %s, token says (%g,%g,%g)", k, str(m[k]).c_str(), want[0], want[1], want[2]);
    }
    CHECK_MSG(m.count("header_height") && std::fabs(m["header_height"].v[0] - t.headerHeight) <= 1.0, "after the swap the Header is %s px tall, token says %g",
              m.count("header_height") ? str(m["header_height"]).c_str() : "(none)", t.headerHeight);
    CHECK_MSG(m.count("field_height") && std::fabs(m["field_height"].v[0] - t.widgetHeight) <= 1.0, "after the swap the input field is %s px tall, token says %g",
              m.count("field_height") ? str(m["field_height"]).c_str() : "(none)", t.widgetHeight);
}

TEST(every_element_changes_when_its_token_changes) {
    // The anti-vacuity check, and the one that catches hard-coded literals directly: an element
    // drawn from a literal is IDENTICAL in both builds.
    Measure a, b; std::string why;
    CHECK_MSG(run("EMBER_PROBE_DEFAULT", a, why) && run("EMBER_PROBE_SWAPPED", b, why), "%s", why.c_str());
    if (a.empty() || b.empty()) return;
    std::vector<std::string> keys(std::begin(kPixelKeys), std::end(kPixelKeys));
    keys.push_back("text_primary"); keys.push_back("text_disabled");
    for (const std::string& k : keys) {
        CHECK_MSG(a.count(k) && b.count(k), "'%s' missing from a probe", k.c_str());
        if (!a.count(k) || !b.count(k)) continue;
        const double d = sumDelta(a[k], b[k]);
        CHECK_MSG(d >= 15.0, "'%s' is %s in BOTH builds after every token changed: it is drawn from something that is not the theme", k.c_str(),
                  str(a[k]).c_str());
    }
    CHECK_MSG(a["header_height"].v[0] != b["header_height"].v[0], "the Header height did not change with its token (%s)", str(a["header_height"]).c_str());
    CHECK_MSG(a["field_height"].v[0] != b["field_height"].v[0], "the input-field height did not change with its token (%s)", str(a["field_height"]).c_str());
}

TEST(input_field_height_matches_the_token_before_and_after_the_first_frame) {
    // applyTheme() runs during Editor::init(), before ImGui's first NewFrame.
    // Its base-font fallback must produce the same token-sized field as
    // reapplying the theme after a frame has established scaled font state.
    Measure def, swp; std::string why;
    CHECK_MSG(run("EMBER_PROBE_DEFAULT", def, why) && run("EMBER_PROBE_SWAPPED", swp, why), "%s", why.c_str());
    if (def.empty() || swp.empty()) return;
    const Theme d = fromDefaults(), s = fromSwap();
    CHECK_MSG(def.count("field_height") && std::fabs(def["field_height"].v[0] - d.widgetHeight) <= 1.0,
              "at startup the default field is %s px, token says %g", def.count("field_height") ? str(def["field_height"]).c_str() : "(none)", d.widgetHeight);
    CHECK_MSG(swp.count("field_height") && std::fabs(swp["field_height"].v[0] - s.widgetHeight) <= 1.0,
              "at startup the swapped field is %s px, token says %g", swp.count("field_height") ? str(swp["field_height"]).c_str() : "(none)", s.widgetHeight);
    CHECK_MSG(def.count("field_height_after_first_frame") && std::fabs(def["field_height_after_first_frame"].v[0] - d.widgetHeight) <= 1.0,
              "after first frame the default field is %s px, token says %g", def.count("field_height_after_first_frame") ? str(def["field_height_after_first_frame"]).c_str() : "(none)", d.widgetHeight);
    CHECK_MSG(swp.count("field_height_after_first_frame") && std::fabs(swp["field_height_after_first_frame"].v[0] - s.widgetHeight) <= 1.0,
              "after first frame the swapped field is %s px, token says %g", swp.count("field_height_after_first_frame") ? str(swp["field_height_after_first_frame"]).c_str() : "(none)", s.widgetHeight);
    std::printf("       field height: at startup %s px, after the first frame %s px (token %g)\n", str(def["field_height"]).c_str(),
                str(def["field_height_after_first_frame"]).c_str(), d.widgetHeight);
}

HARNESS_MAIN()
