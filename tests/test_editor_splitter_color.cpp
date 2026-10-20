// tests/test_editor_splitter_color.cpp - owned by Verity / QA. Item 141.
//
// Contract under test: while the user drags a dock splitter, the ACTIVE splitter
// line is drawn in neutral chrome, not in the selection accent (orange). Blender
// highlights an active region edge without using the selection colour.
//
// HOW (real pixels, real binary): launches the actual Ember3D.exe with the app's
// own opt-in hook -
//     EMBER_SELFTEST_SPLITTER=1  queue a real ImGui drag of the viewport/right
//                                column splitter (frames 6..22, 70px to the left)
//     EMBER_EDITOR_FRAME_DUMP=p  write the COMPLETE back buffer (all ImGui
//                                chrome included) at frame 22
// and then analyses the BMP WITHOUT trusting the author's sample coordinate: it
// finds the active splitter itself, by scanning for a tall column of one colour
// that the idle layout does not contain.
//
// Needs a GL context, so it is GPU-gated: registered only under
// EMBER_ENABLE_GPU_TESTS and run with --gpu. The exe path comes from EMBER_EXE.
#include "harness.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

struct Rgb { int r = 0, g = 0, b = 0; };

struct Image {
    int w = 0, h = 0;
    std::vector<Rgb> px;   // top-down
    bool ok = false;
    std::string why;
    Rgb at(int x, int y) const { return px[std::size_t(y) * std::size_t(w) + std::size_t(x)]; }
};

std::uint32_t le32(const std::vector<unsigned char>& d, std::size_t o) {
    return std::uint32_t(d[o]) | (std::uint32_t(d[o + 1]) << 8) | (std::uint32_t(d[o + 2]) << 16) |
           (std::uint32_t(d[o + 3]) << 24);
}

Image readBmp(const std::string& path) {
    Image im;
    std::ifstream f(path, std::ios::binary);
    if (!f) { im.why = "cannot open " + path; return im; }
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') { im.why = "not a BMP"; return im; }
    const std::uint32_t off = le32(d, 10);
    const int w = int(le32(d, 18));
    const int hRaw = int(le32(d, 22));
    const int bpp = int(d[28] | (d[29] << 8));
    const int h = hRaw < 0 ? -hRaw : hRaw;
    if ((bpp != 24 && bpp != 32) || w <= 0 || h <= 0) { im.why = "unsupported BMP (bpp/size)"; return im; }
    const std::size_t stride = (std::size_t(w) * std::size_t(bpp / 8) + 3u) & ~std::size_t(3);
    if (d.size() < off + stride * std::size_t(h)) { im.why = "BMP truncated"; return im; }
    im.w = w; im.h = h; im.px.resize(std::size_t(w) * std::size_t(h));
    for (int row = 0; row < h; ++row) {
        const int y = hRaw < 0 ? row : (h - 1 - row);          // positive height = bottom-up
        const unsigned char* p = &d[off + stride * std::size_t(row)];
        for (int x = 0; x < w; ++x) {
            const unsigned char* q = p + x * (bpp / 8);        // BGR(A)
            im.px[std::size_t(y) * std::size_t(w) + std::size_t(x)] = {q[2], q[1], q[0]};
        }
    }
    im.ok = true;
    return im;
}

bool near(const Rgb& a, const Rgb& b, int tol) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

// Orange-ish: the default selection accent is (237,115,26). Anything with a hot
// red channel, a mid green and a low blue is "selection colour" to a human eye.
bool accentLike(const Rgb& c) { return c.r >= 170 && c.g >= 70 && c.g <= 170 && c.b <= 90 && c.r - c.b >= 110; }

int spread(const Rgb& c) {
    const int hi = std::max(c.r, std::max(c.g, c.b)), lo = std::min(c.r, std::min(c.g, c.b));
    return hi - lo;
}

std::string str(const Rgb& c) {
    char b[48]; std::snprintf(b, sizeof b, "(%d,%d,%d)", c.r, c.g, c.b); return b;
}

} // namespace

TEST_GPU(active_splitter_is_neutral_chrome_not_selection_orange) {
    const char* exeEnv = std::getenv("EMBER_EXE");
    CHECK_MSG(exeEnv && *exeEnv, "EMBER_EXE is not set (CTest sets it to the Ember3D target)");
    if (!exeEnv || !*exeEnv) return;
    CHECK_MSG(std::filesystem::exists(exeEnv), "Ember3D.exe not found at %s", exeEnv);
    if (!std::filesystem::exists(exeEnv)) return;

    const auto dir = std::filesystem::temp_directory_path() / "opencode";
    std::filesystem::create_directories(dir);
    const std::string bmp = (dir / "item141_splitter.bmp").string();
    std::filesystem::remove(bmp);

    _putenv_s("EMBER_SELFTEST_SPLITTER", "1");
    _putenv_s("EMBER_EDITOR_FRAME_DUMP", bmp.c_str());
    const std::string cmd = std::string("\"\"") + exeEnv + "\" --frames 22 --exit\"";
    const int rc = std::system(cmd.c_str());
    _putenv_s("EMBER_SELFTEST_SPLITTER", "");
    _putenv_s("EMBER_EDITOR_FRAME_DUMP", "");
    CHECK_MSG(rc == 0, "Ember3D --frames 22 --exit returned %d", rc);

    const Image im = readBmp(bmp);
    CHECK_MSG(im.ok, "frame dump unusable: %s", im.why.c_str());
    if (!im.ok) return;
    std::printf("       frame %dx%d\n", im.w, im.h);

    // Vertical band between the top menu bar (~19px) and the status bar (32px),
    // clear of the Timeline that spans the bottom (the splitter ends above it).
    const int y0 = 30, y1 = int(im.h * 0.72f) - 30;   // 30 .. ~488 on a 720px frame
    // Find a column whose pixels are one non-dark, non-grey-chrome colour for
    // almost the whole band: that is a highlighted separator.
    int splitterX0 = -1, splitterX1 = -1;
    Rgb active{};
    for (int x = 0; x < im.w; ++x) {
        const Rgb ref = im.at(x, (y0 + y1) / 2);
        if (ref.r + ref.g + ref.b < 3 * 90) continue;                 // viewport/3D/dark chrome
        int same = 0;
        for (int y = y0; y < y1; ++y) if (near(im.at(x, y), ref, 2)) ++same;
        if (same >= int(0.9 * (y1 - y0))) {
            if (splitterX0 < 0) { splitterX0 = x; active = ref; }
            splitterX1 = x;
        }
    }
    CHECK_MSG(splitterX0 >= 0,
              "no highlighted (active) splitter column found - either the drag never started or the "
              "separator is not drawn in a distinct colour");
    if (splitterX0 < 0) return;
    std::printf("       active splitter: x=%d..%d colour %s\n", splitterX0, splitterX1, str(active).c_str());

    // 1. The drag really moved it: the default splitter sits at ~78%% of the width (996 on 1280).
    const int defaultX = int(im.w * 0.78f);
    CHECK_MSG(splitterX0 < defaultX - 40, "splitter at x=%d did not move left from its default ~%d", splitterX0,
              defaultX);
    // 2. It is a thin line, not a filled panel.
    CHECK_MSG(splitterX1 - splitterX0 <= 5, "highlighted column is %d px wide - that is not a splitter line",
              splitterX1 - splitterX0 + 1);
    // 3. THE CONTRACT: not selection orange; low-saturation chrome.
    CHECK_MSG(!accentLike(active), "active splitter colour %s is selection-orange", str(active).c_str());
    CHECK_MSG(spread(active) <= 60, "active splitter colour %s is saturated (spread %d); chrome should be neutral",
              str(active).c_str(), spread(active));
    // 4. Nothing orange within 4px either side of the line along its whole length.
    int orange = 0, firstOrangeX = -1, firstOrangeY = -1;
    for (int x = std::max(0, splitterX0 - 4); x <= std::min(im.w - 1, splitterX1 + 4); ++x)
        for (int y = y0; y < y1; ++y)
            if (accentLike(im.at(x, y))) { if (orange++ == 0) { firstOrangeX = x; firstOrangeY = y; } }
    CHECK_MSG(orange == 0, "%d orange pixel(s) beside the active splitter, first at (%d,%d) %s", orange,
              firstOrangeX, firstOrangeY, orange ? str(im.at(firstOrangeX, firstOrangeY)).c_str() : "");
    // 5. Pin the documented token so a silent re-theme is noticed (update on purpose).
    CHECK_MSG(near(active, Rgb{184, 201, 224}, 3),
              "active splitter is %s; the item-141 neutral chrome token was (184,201,224) - if the theme "
              "changed on purpose, update this pin", str(active).c_str());
}

HARNESS_MAIN()
