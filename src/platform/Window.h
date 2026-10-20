// src/platform/Window.h — ember::platform::Window
// Contract: ARCHITECTURE.md §4.2. Owned by Kiln · Platform.
#pragma once

#include <string>

// Match GLFW's opaque struct name so we can hold one without including GLFW
// in this header (GLFW declares: typedef struct GLFWwindow GLFWwindow;).
struct GLFWwindow;

namespace ember::platform {

struct WindowConfig {
    int         width  = 1280;
    int         height = 720;
    std::string title  = "Ember3D";
    bool        vsync  = true;
    bool        visible = true;
};

// Owns the OS window and the OpenGL 4.5 core context.
// create() initialises GLFW (once per process), makes the context current and
// loads the GL 4.5 entry points (callers never call loadGLFunctions()
// themselves — the ordering lives here).
// destroy() releases everything and is idempotent; the destructor calls it.
// Smoke-test hook: set EMBER_AUTO_CLOSE_SECONDS=<n> and the window closes
// itself n seconds after creation, so CI can prove open/close without a human.
class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool  create(const WindowConfig&);
    void  destroy();

    bool  shouldClose() const;
    void  requestClose();               // extra (not in §4.2): programmatic close

    void  pollEvents();
    void  swapBuffers();

    int   width()  const;               // framebuffer size (pixels)
    int   height() const;

    // LOGICAL window size (D17 window coordinates, = glfwGetWindowSize) — the
    // space of the input rect and the cursor. width()/height() are framebuffer
    // pixels; at HiDPI these differ by the scale factor, so this is the only
    // HiDPI-correct size for comparisons outside platform (e.g. App's staleness
    // check). Additive read-only accessor, cleared with Ash (policy: no
    // decision/re-freeze needed).
    int   windowWidth() const;
    int   windowHeight() const;

    // F11: toggle borderless fullscreen on the window's monitor (D17 defect 1;
    // no in-app fullscreen existed before). Safe to call at any time.
    void  toggleFullscreen();
    bool  isFullscreen() const;

    void* nativeHandle() const;         // GLFWwindow*, for backends (ImGui etc.)

    bool  isCreated() const { return window_ != nullptr; }

    // Extra (not in §4.2): viewport rectangle in window coordinates, so
    // Input::cursorOverViewport() knows where the 3D viewport is. Defaults to
    // the whole window. The editor sets this once its layout is known.
    void  setViewportRect(int x, int y, int w, int h);

private:
    GLFWwindow* window_ = nullptr;

    bool fullscreen_ = false;
    int  restoreX_ = 0, restoreY_ = 0, restoreW_ = 1280, restoreH_ = 720;
    bool rectDirty_ = true;             // resync the input rect on next poll
};

// Monotonic seconds since GLFW initialisation (0.0 before the first Window).
double timeSeconds();

} // namespace ember::platform
