// src/platform/Input.h — ember::platform::Input
// Contract: ARCHITECTURE.md §4.2. Owned by Kiln · Platform.
#pragma once

#include "core/Math.h" // ember::Vec2 — core owns shared types

namespace ember::platform {

// Key values equal GLFW key codes so the feed path is a direct array index.
// The names in §4.2 (W A S D Q E G R Delete Escape …) are all here.
enum class Key : int {
    // letters
    A = 65, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    // digits (top row)
    Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    // Numpad (D35 request: view keys live HERE, freeing the top row for the
    // editor's select modes). Values equal GLFW_KEY_KP_0..9 / _KP_DECIMAL, so
    // the direct-index feed needs no mapping — onKey receives GLFW codes as-is.
    KP_0 = 320, KP_1, KP_2, KP_3, KP_4, KP_5, KP_6, KP_7, KP_8, KP_9,
    KP_Decimal = 330,   // Blender: numpad-period = frame/coordinate entry
    // punctuation / whitespace
    Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    Semicolon = 59, Equals = 61,
    GraveAccent = 96,
    // editing / navigation
    Escape = 256, Enter = 257, Tab = 258, Backspace = 259, Insert = 260,
    Delete = 261, Right = 262, Left = 263, Down = 264, Up = 265,
    PageUp = 266, PageDown = 267, Home = 268, End = 269,
    CapsLock = 280, PrintScreen = 283, Pause = 284,
    // function keys
    F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    // modifiers
    LeftShift = 340, LeftCtrl = 341, LeftAlt = 342, LeftSuper = 343,
    RightShift = 344, RightCtrl = 345, RightAlt = 346, RightSuper = 347,

    // ARCHITECTURE §4.2 lists both `S` and `S_`; keep S_ as an alias of S so
    // code written against either name compiles.
    S_ = 83,
};

enum class MouseButton : int { Left = 0, Right = 1, Middle = 2 };

// Process-wide singleton (see input() below). Window::pollEvents() drives the
// frame lifecycle; the on*() methods are the feed API used by Window's GLFW
// callbacks — do not call them from module code.
class Input {
public:
    static constexpr int kMaxKeys = 512; // GLFW key-code range

    bool keyDown(Key) const;
    bool keyPressed(Key) const;          // edge: pressed since last pollEvents()
    bool mouseDown(MouseButton) const;
    bool mousePressed(MouseButton) const;

    Vec2 mousePosition() const;          // WINDOW coords (D17 canonical space)
    Vec2 mouseDelta() const;             // movement during the last pollEvents()
    Vec2 scrollDelta() const;            // wheel ticks during the last pollEvents()

    bool cursorOverViewport() const;     // inside the viewport rect (see setViewportRect)

    // The rect as last set, plus the window size it was captured at — a stale
    // rect (captured window size != current window size) is then a pure unit
    // test with no GL context, per Ash's D17 review note.
    struct ViewportRect {
        int x = 0, y = 0, w = 0, h = 0;
        int windowWidth = 0, windowHeight = 0;   // 0 = caller did not supply it
    };
    ViewportRect viewportRect() const;

    // True when the rect was captured for a DIFFERENT window size than the one
    // given — the pure form of the stale-rect regression (WHAT-REMAINS defect 1),
    // unit-testable with no GLFW window and no GL context. Returns false when
    // the capture size is unknown (caller did not supply one) or no live size
    // is given, since staleness then cannot be judged.
    bool rectIsStaleFor(int windowWidth, int windowHeight) const;

    // -- engine-internal: feed + frame lifecycle ---------------------------
    void reset();                        // clear all state (fresh window)
    void beginFrame();
    void endFrame();

    void onKey(int glfwKey, bool down);
    void onMouseButton(int glfwButton, bool down);
    void onCursorPos(float x, float y);
    void onScroll(float dx, float dy);

    // Viewport rectangle in WINDOW coordinates (D17: the canonical input
    // space — same space as glfwGetCursorPos and ImGui, origin top-left,
    // y down). Defaults to the whole window; the editor updates it through
    // App once its layout is known.
    void setViewportRect(int x, int y, int w, int h,
                         int windowWidth = -1, int windowHeight = -1);

private:
    bool  keyDown_[kMaxKeys]      = {};
    bool  keyPressed_[kMaxKeys]   = {};
    bool  mouseDown_[8]           = {};
    bool  mousePressed_[8]        = {};

    Vec2  mousePosition_          = {0.0f, 0.0f};
    Vec2  frameStartPos_          = {0.0f, 0.0f};
    Vec2  mouseDelta_             = {0.0f, 0.0f};
    Vec2  scrollDelta_            = {0.0f, 0.0f};
    bool  hasMousePosition_       = false;

    int   viewportX_ = 0, viewportY_ = 0, viewportW_ = 0, viewportH_ = 0;
    int   viewportCaptureW_ = 0, viewportCaptureH_ = 0;
};

// The process-wide input state that Window feeds. (§4.2 shows Input as a bare
// class; a single instance behind this accessor is the wiring.)
Input& input();

} // namespace ember::platform
