// tests/test_platform_window.cpp — owned by Verity · QA.
//
// THE REGRESSION TEST FOR WHAT-REMAINS DEFECT #1, at the level where the bug
// actually lived. HONESTY NOTE carried over from test_platform_input.cpp:
// Input::setViewportRect was always correct when called — the defect was that
// Window captured the rect ONCE at creation (the only caller of
// setViewportRect) and never updated it, because there was no size callback.
// So the test that reproduces the original bug must drive a REAL window through
// a REAL resize and observe input().viewportRect(). That needs a live window,
// hence --gpu; the pure contract lives in test_platform_input.cpp.
//
// Pre-fix behaviour these tests fail against (verified by running them against
// a scratch tree with the size callbacks removed):
//   create 1280x720 -> rect captured at 1280x720
//   resize to 2560x1440 -> rect STILL 1280x720 (stale)
//   cursorOverViewport() at (1600,900) -> false, but it should be true
//   => orbit only works in the original top-left patch. Exactly the report.
//
// Post-fix (D17 + Kiln's syncViewportRect): both glfwSetWindowSizeCallback and
// glfwSetFramebufferSizeCallback resync from glfwGetWindowSize (WINDOW coords,
// the canonical space), plus create/first-poll/toggleFullscreen resync.
#include "harness.h"

#include "platform/Input.h"
#include "platform/Window.h"

#include < GLFW/glfw3.h >   // for the explicit resize drive; Window.h only forward-declares

using ember::platform::Input;
using ember::platform::Window;
using ember::platform::WindowConfig;
using ember::platform::input;

namespace {

// Drive a real resize the way the OS would (maximise / fullscreen / DPI change)
// and pump events so GLFW delivers the size callbacks to Window's handlers.
void realResize(Window& w, int newW, int newH) {
    GLFWwindow* raw = static_cast<GLFWwindow*>(w.nativeHandle());
    glfwSetWindowSize(raw, newW, newH);
    for (int i = 0; i < 5; ++i) w.pollEvents();   // callbacks fire during poll
}

// QA NOTE (a bug in MY first version of this file, caught by running it against
// the pre-fix tree): Window::pollEvents() consumes a one-shot rectDirty_ flag
// that is set at create — the FIRST poll re-syncs the rect regardless of the
// size callbacks. My original test resized before ever polling, so that
// deferred create-time sync fired on the POST-resize size and masked the very
// staleness under test: the test passed against the broken tree and proved
// nothing. Settling the first frames at the CREATION size first (as the real
// app does, many frames before any resize) is what makes the resize depend on
// the callbacks/self-heal that the pre-fix tree lacks.
void settle(Window& w) {
    for (int i = 0; i < 3; ++i) w.pollEvents();
}

} // namespace

// A hidden window: no visible flash, but a real GLFW window with real size
// callbacks. Config.visible=false is supported by WindowConfig.
struct ScopedHiddenWindow {
    Window w;
    bool ok = false;
    ScopedHiddenWindow(int width, int height) {
        WindowConfig cfg;
        cfg.width = width;
        cfg.height = height;
        cfg.visible = false;
        cfg.vsync = false;
        ok = w.create(cfg);
    }
    ~ScopedHiddenWindow() { w.destroy(); }
};

TEST(rect_tracks_a_real_window_resize) {
    // THE regression test. Fails on the pre-fix tree (no size callback).
    ScopedHiddenWindow win(1280, 720);
    if (!win.ok) {
        harness::reportFailure(__FILE__, __LINE__,
            "could not create a hidden window - GPU required for this test "
            "(run the binary with --gpu)");
        return;
    }

    // Creation must have captured the rect.
    Input& in = input();
    const auto atCreate = in.viewportRect();
    CHECK_EQ(atCreate.windowWidth, 1280);
    CHECK_EQ(atCreate.windowHeight, 720);

    // Run the first frames at the CREATION size so the one-shot rectDirty_
    // resync is consumed before we resize (see settle() note above).
    settle(win.w);

    // A cursor outside the ORIGINAL size but inside the NEW size.
    in.onCursorPos(1600.0f, 900.0f);

    realResize(win.w, 2560, 1440);

    // Do NOT assert against the REQUESTED size: the OS may clamp it (a 2560x1440
    // request became 2560x1421 here, minus the taskbar). Assert the rect tracked
    // whatever the window ACTUALLY became — that is the contract. Measured once
    // against the requested size and failed; querying the truth fixed it.
    GLFWwindow* raw = static_cast<GLFWwindow*>(win.w.nativeHandle());
    int actualW = 0, actualH = 0;
    glfwGetWindowSize(raw, &actualW, &actualH);

    const auto after = in.viewportRect();
    if (after.windowWidth != actualW || after.windowHeight != actualH)
        harness::reportFailure(__FILE__, __LINE__,
            "viewport rect did not track the resize: rect is " +
            std::to_string(after.windowWidth) + "x" + std::to_string(after.windowHeight) +
            " but the window is actually " + std::to_string(actualW) + "x" +
            std::to_string(actualH) + " (stale rect = the fullscreen orbit bug)");
    ++harness::g_checks;

    // The rect must have CHANGED from the creation-time value at all.
    CHECK(after.windowWidth != atCreate.windowWidth ||
          after.windowHeight != atCreate.windowHeight);

    // And the hit-test verdict must agree with the NEW geometry: the cursor at
    // (1600,900) is inside the enlarged viewport and outside the old 1280x720.
    if (!in.cursorOverViewport())
        harness::reportFailure(__FILE__, __LINE__,
            "cursor (1600,900) judged outside the resized viewport - orbit would "
            "be dead outside the original patch, which is the reported symptom");
    ++harness::g_checks;
}

TEST(rect_shrinks_back_after_resize_down) {
    ScopedHiddenWindow win(1600, 1000);
    if (!win.ok) { harness::reportFailure(__FILE__, __LINE__, "no window (needs --gpu)"); return; }
    Input& in = input();
    settle(win.w);                       // consume the one-shot create resync first

    realResize(win.w, 800, 600);
    const auto r = in.viewportRect();
    CHECK_EQ(r.windowWidth, 800);
    CHECK_EQ(r.windowHeight, 600);

    // A cursor beyond the shrunken size must now miss.
    in.onCursorPos(1200.0f, 800.0f);
    CHECK(!in.cursorOverViewport());
}

TEST(rect_is_in_window_space_not_framebuffer_space_under_dpi) {
    // D17: the rect comes from glfwGetWindowSize, never framebuffer pixels.
    // Under OS scaling the two differ; the hit-test must use WINDOW coords so
    // it agrees with the cursor (also window coords) and with ImGui.
    ScopedHiddenWindow win(1280, 720);
    if (!win.ok) { harness::reportFailure(__FILE__, __LINE__, "no window (needs --gpu)"); return; }
    Input& in = input();

    GLFWwindow* raw = static_cast<GLFWwindow*>(win.w.nativeHandle());
    int winW = 0, winH = 0, fbW = 0, fbH = 0;
    glfwGetWindowSize(raw, &winW, &winH);
    glfwGetFramebufferSize(raw, &fbW, &fbH);

    const auto r = in.viewportRect();
    // The rect must match the WINDOW size, whatever the framebuffer size is.
    if (r.windowWidth != winW || r.windowHeight != winH)
        harness::reportFailure(__FILE__, __LINE__,
            "viewport rect is " + std::to_string(r.windowWidth) + "x" +
            std::to_string(r.windowHeight) + " but the WINDOW is " +
            std::to_string(winW) + "x" + std::to_string(winH) +
            " (fb is " + std::to_string(fbW) + "x" + std::to_string(fbH) +
            ") - D17 requires window coordinates");
    ++harness::g_checks;

    // Note: on a 1x display fb == window and this test cannot distinguish the
    // spaces; it still guards the wiring, and it bites on a scaled display.
    if (fbW == winW && fbH == winH)
        std::printf("    (note: 1x scale here, fb==window; the space check is "
                    "inert on this display but active on DPI-scaled ones)\n");
}

TEST(rect_resyncs_on_poll_before_the_next_frame) {
    // Window.cpp also resyncs on poll via rectDirty_, so a resize delivered by
    // the OS between polls is corrected within a frame, not one frame late.
    ScopedHiddenWindow win(1280, 720);
    if (!win.ok) { harness::reportFailure(__FILE__, __LINE__, "no window (needs --gpu)"); return; }
    Input& in = input();
    settle(win.w);                       // consume the one-shot create resync first

    GLFWwindow* raw = static_cast<GLFWwindow*>(win.w.nativeHandle());
    glfwSetWindowSize(raw, 1920, 1080);
    win.w.pollEvents();   // one poll must be enough
    const auto r = in.viewportRect();
    CHECK_EQ(r.windowWidth, 1920);
    CHECK_EQ(r.windowHeight, 1080);
}

HARNESS_MAIN()
