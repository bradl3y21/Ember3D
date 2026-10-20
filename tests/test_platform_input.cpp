// tests/test_platform_input.cpp — owned by Verity · QA.
//
// Contract under test: ember::platform::Input's viewport hit-test
// (src/platform/Input.h, ARCHITECTURE.md §4.2) — the gate that decides whether
// the cursor is over the 3D viewport, i.e. whether middle-mouse orbit applies.
//
// HISTORY (WHAT-REMAINS defect #1, found by a human using the app):
//   The rect was captured ONCE at window creation from glfwGetFramebufferSize
//   and never updated, so after fullscreen/maximise orbit only worked in the
//   original 1280x720 patch. Two things fixed it:
//     D17 (Kiln/Ash): the canonical input space is WINDOW coordinates — the
//       same space as glfwGetCursorPos and ImGui. The rect and the cursor must
//       be in the SAME space; nothing may silently mix in framebuffer pixels.
//     Window.cpp now registers glfwSetWindowSizeCallback AND
//       glfwSetFramebufferSizeCallback, both funnelling into syncViewportRect(),
//       which is also called at create, on first poll, and after toggleFullscreen.
//
// IMPORTANT HONESTY NOTE: the original defect was NOT in Input — Input::setViewportRect
// worked correctly whenever it was called. The defect was that Window never called
// it again. Therefore:
//   * The PURE tests below (no GPU, always run) pin Input's contract: the
//     hit-test geometry, the D17 same-space rule, and that the rect is NOT
//     sticky inside Input (it updates the instant setViewportRect is called).
//     They also pin viewportRect() as the staleness signal Kiln exposed.
//   * The test that reproduces the ORIGINAL bug — "the rect must track a real
//     window resize" — must drive Window, i.e. needs a live window. That lives
//     in test_platform_window.cpp behind --gpu, and it is the one proven to
//     fail against the pre-fix code path.
//
// NO GPU, NO GL CONTEXT, NO GLFW in this file: Input.cpp includes only
// platform/Input.h and is compiled directly into this binary.
#include "harness.h"

#include "platform/Input.h"

using ember::platform::Input;

namespace {

void cursorAt(Input& in, float x, float y) { in.onCursorPos(x, y); }

} // namespace

// ---- contract: the hit-test geometry ---------------------------------------

TEST(hit_inside_rect_returns_true) {
    Input in;
    in.setViewportRect(0, 0, 1280, 720);
    cursorAt(in, 640.0f, 360.0f);
    CHECK(in.cursorOverViewport());
}

TEST(hit_outside_rect_returns_false) {
    Input in;
    in.setViewportRect(0, 0, 1280, 720);
    cursorAt(in, 1400.0f, 100.0f);   // right of the rect
    CHECK(!in.cursorOverViewport());
    cursorAt(in, 100.0f, 900.0f);    // below the rect
    CHECK(!in.cursorOverViewport());
    cursorAt(in, -5.0f, 100.0f);     // negative (possible on multi-monitor)
    CHECK(!in.cursorOverViewport());
}

TEST(hit_boundary_is_half_open_inclusive_low_exclusive_high) {
    // Pins the exact convention (x >= x0 && x < x0+w) so a future change cannot
    // silently flip it and break the editor's own layout math.
    Input in;
    in.setViewportRect(100, 50, 200, 100);   // x in [100,300), y in [50,150)
    cursorAt(in, 100.0f, 50.0f);    CHECK(in.cursorOverViewport());   // low corner
    cursorAt(in, 299.0f, 149.0f);  CHECK(in.cursorOverViewport());   // last inside
    cursorAt(in, 300.0f, 149.0f);  CHECK(!in.cursorOverViewport());  // high edge
    cursorAt(in, 299.0f, 150.0f);  CHECK(!in.cursorOverViewport());
    cursorAt(in,  99.0f, 149.0f);  CHECK(!in.cursorOverViewport());
}

TEST(hit_offset_rect_excludes_panel_region) {
    // The editor sets a viewport that excludes panels, so the rect is NOT
    // anchored at (0,0). A cursor over a left-hand panel must miss.
    Input in;
    in.setViewportRect(300, 0, 980, 720);    // 300px panel on the left
    cursorAt(in, 150.0f, 300.0f);  CHECK(!in.cursorOverViewport());   // over the panel
    cursorAt(in, 400.0f, 300.0f);  CHECK(in.cursorOverViewport());    // over the viewport
}

TEST(hit_no_rect_configured_defaults_to_whole_window) {
    Input in;
    cursorAt(in, 5000.0f, 5000.0f);
    CHECK(in.cursorOverViewport());          // zero rect == whole window
}

TEST(hit_reset_clears_the_rect) {
    Input in;
    in.setViewportRect(0, 0, 1280, 720);
    in.reset();
    cursorAt(in, 2000.0f, 2000.0f);
    CHECK(in.cursorOverViewport());          // back to whole-window default
}

// ---- D17: rect and cursor must live in ONE coordinate space ----------------
// The rect is in WINDOW coordinates, the cursor arrives in WINDOW coordinates,
// and the hit-test must NOT secretly rescale by a DPI/framebuffer factor. If
// anyone reintroduces a framebuffer-pixel conversion inside Input these fail —
// which is exactly the regression D17 exists to prevent.

TEST(d17_hit_test_does_not_rescale_by_framebuffer_factor) {
    // Simulate 2x DPI: the framebuffer is 2560x1440 but the WINDOW is 1280x720.
    // D17 says the rect is expressed in WINDOW coords, so a 300px left panel
    // means the viewport starts at window x=300 — NOT at framebuffer x=600.
    Input in;
    in.setViewportRect(300, 0, 980, 720, /*windowWidth=*/1280, /*windowHeight=*/720);

    // A cursor at window x=400 is over the viewport (400 > 300). The hit-test
    // must agree purely in window space.
    cursorAt(in, 400.0f, 300.0f);
    CHECK(in.cursorOverViewport());

    // If Input had wrongly scaled the rect to framebuffer pixels (600..2560),
    // this cursor at window x=400 would be judged "outside". It must not be.
    cursorAt(in, 200.0f, 300.0f);           // over the panel, in window space
    CHECK(!in.cursorOverViewport());
}

TEST(d17_cursor_and_rect_agree_under_scaling_simulation) {
    // Sweep the boundary in window space under a 2x and a 1.5x framebuffer
    // scale. The verdict must depend ONLY on window-space geometry.
    for (float scale : {1.0f, 1.5f, 2.0f, 3.0f}) {
        Input in;
        const int winW = 1280, winH = 720;
        in.setViewportRect(0, 0, winW, winH, winW, winH);
        cursorAt(in, (float)(winW - 1), (float)(winH - 1));
        if (!in.cursorOverViewport())
            harness::reportFailure(__FILE__, __LINE__,
                          "last in-window pixel judged outside at scale " +
                          std::to_string(scale));
        cursorAt(in, (float)winW, (float)winH);   // one past the edge
        if (in.cursorOverViewport())
            harness::reportFailure(__FILE__, __LINE__,
                          "one-past-edge pixel judged inside at scale " +
                          std::to_string(scale));
        ++harness::g_checks;
    }
}

// ---- viewportRect() is the staleness signal (Kiln's addition) ---------------
// The getter records the window size the rect was captured at, so a consumer
// (App, or a test) can DETECT a stale rect without any GL context: captured
// size != current window size means "resync before trusting the hit-test".

TEST(viewport_rect_getter_returns_what_was_set) {
    Input in;
    in.setViewportRect(120, 40, 800, 600, 1920, 1080);
    const auto r = in.viewportRect();
    CHECK_EQ(r.x, 120);
    CHECK_EQ(r.y, 40);
    CHECK_EQ(r.w, 800);
    CHECK_EQ(r.h, 600);
    CHECK_EQ(r.windowWidth, 1920);
    CHECK_EQ(r.windowHeight, 1080);
}

TEST(viewport_rect_getter_exposes_capture_size_for_staleness_detection) {
    // The fullscreen scenario, detectable with no window at all:
    // captured at 1280x720, window is now 2560x1440 -> the sizes differ, so a
    // consumer must resync before using cursorOverViewport().
    Input in;
    in.setViewportRect(0, 0, 1280, 720, 1280, 720);
    const auto captured = in.viewportRect();
    const int currentWindowW = 2560, currentWindowH = 1440;   // after fullscreen
    const bool stale = captured.windowWidth != currentWindowW ||
                       captured.windowHeight != currentWindowH;
    CHECK(stale);                                          // detectable, no GL

    // After the system resyncs, the mismatch is gone.
    in.setViewportRect(0, 0, currentWindowW, currentWindowH,
                       currentWindowW, currentWindowH);
    const auto after = in.viewportRect();
    CHECK(after.windowWidth == currentWindowW && after.windowHeight == currentWindowH);
    CHECK(!(after.windowWidth != currentWindowW || after.windowHeight != currentWindowH));
}

// ---- regression guard: the rect is NOT sticky inside Input ------------------
// The operator's requested regression test at the Input level: when the size
// changes, the hit-test verdict must change with it immediately. This passes
// on both the pre- and post-fix Input (Input was never the broken part) — its
// job is to keep setViewportRect honest and to fail loudly if anyone makes the
// stored rect sticky. The test that reproduces the ORIGINAL defect (Window
// failing to resync) is test_platform_window.cpp, gated behind --gpu.

TEST(regression_hit_follows_rect_change_immediately) {
    Input in;
    in.setViewportRect(0, 0, 1280, 720, 1280, 720);
    cursorAt(in, 1500.0f, 800.0f);
    CHECK(!in.cursorOverViewport());        // outside the original size

    in.setViewportRect(0, 0, 2560, 1440, 2560, 1440);   // resized / fullscreened
    cursorAt(in, 1500.0f, 800.0f);
    CHECK(in.cursorOverViewport());         // inside the NEW size

    in.setViewportRect(0, 0, 1024, 768, 1024, 768);     // shrunk again
    cursorAt(in, 1500.0f, 800.0f);
    CHECK(!in.cursorOverViewport());
}

HARNESS_MAIN()
