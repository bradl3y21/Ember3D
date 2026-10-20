#include "platform/Window.h"
#include "platform/Input.h"
#include "platform/GLLoader.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>

namespace ember::platform {
namespace {

Window* g_window = nullptr; // the window callbacks feed from

void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "[ember::platform] GLFW error %d: %s\n", code,
                 description ? description : "(no description)");
}

int g_windowCount = 0; // GLFW init/terminate refcount (one window in practice)

// M0 smoke-test hook: EMBER_AUTO_CLOSE_SECONDS=<n> makes the window close
// itself n seconds after creation, so "window opens and closes cleanly" can be
// verified without a human clicking the X. Read once, at first create().
double g_autoCloseAt = -1.0;

void installAutoCloseHook() {
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    if (const char* env = std::getenv("EMBER_AUTO_CLOSE_SECONDS")) {
        const double seconds = std::atof(env);
        if (seconds > 0.0) {
            g_autoCloseAt = glfwGetTime() + seconds;
            std::printf("[ember::platform] auto-close hook: window will close in %.1fs "
                        "(EMBER_AUTO_CLOSE_SECONDS)\n", seconds);
        }
    }
}

// Bounded-run resize hook: EMBER_SMOKE_RESIZE=<W>x<H> resizes the window once,
// a third of the way through the run, so the SMOKE exercises the size-callback
// path (WHAT-REMAINS defect 1: the rect must follow maximise/fullscreen/DPI).
// Without this a --frames run would never resize, and a regression that breaks
// the callbacks would pass silently. Checked by cmake/RunSmokeSizeCheck.cmake.
int  g_smokeResizeW = 0, g_smokeResizeH = 0;
long g_smokeResizeAtFrame = -1;   // -1 = never
long g_pollFrames = 0;

void installSmokeResizeHook(long totalFramesHint) {
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    if (const char* env = std::getenv("EMBER_SMOKE_RESIZE")) {
        int w = 0, h = 0;
        if (std::sscanf(env, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            g_smokeResizeW = w;
            g_smokeResizeH = h;
            g_smokeResizeAtFrame = totalFramesHint > 0 ? totalFramesHint / 3 : 10;
            std::printf("[ember::platform] smoke resize hook: %dx%d at frame %ld "
                        "(EMBER_SMOKE_RESIZE)\n", w, h, g_smokeResizeAtFrame);
        }
    }
}

void keyCallback(GLFWwindow*, int key, int /*scancode*/, int action, int /*mods*/) {
    if (action == GLFW_PRESS || action == GLFW_RELEASE)
        input().onKey(key, action == GLFW_PRESS);
}

void mouseButtonCallback(GLFWwindow*, int button, int action, int /*mods*/) {
    if (action == GLFW_PRESS || action == GLFW_RELEASE)
        input().onMouseButton(button, action == GLFW_PRESS);
}

void cursorPosCallback(GLFWwindow*, double x, double y) {
    input().onCursorPos(static_cast<float>(x), static_cast<float>(y));
}

void scrollCallback(GLFWwindow*, double dx, double dy) {
    input().onScroll(static_cast<float>(dx), static_cast<float>(dy));
}

// Keep the input viewport rect current. D17: the canonical input space is
// WINDOW (logical) coordinates — the same space as glfwGetCursorPos and ImGui —
// so the rect comes from glfwGetWindowSize, never from framebuffer pixels.
// Called at create, on first pollEvents, and from BOTH size callbacks (either
// fires on maximise / fullscreen / DPI change; before this, the rect was
// captured once and went stale, which is WHAT-REMAINS defect 1).
void syncViewportRect(GLFWwindow* window) {
    if (!window)
        return;
    int winW = 0, winH = 0;
    glfwGetWindowSize(window, &winW, &winH);
    input().setViewportRect(0, 0, winW, winH, winW, winH);
}

void windowSizeCallback(GLFWwindow* window, int, int) {
    syncViewportRect(window);
}

void framebufferSizeCallback(GLFWwindow* window, int, int) {
    syncViewportRect(window);
}

} // namespace

// -- Window ----------------------------------------------------------------

Window::~Window() { destroy(); }

bool Window::create(const WindowConfig& config) {
    if (window_) {
        std::fprintf(stderr, "[ember::platform] Window::create called twice\n");
        return false;
    }

    if (g_windowCount == 0) {
        glfwSetErrorCallback(glfwErrorCallback);
        if (!glfwInit()) {
            std::fprintf(stderr, "[ember::platform] glfwInit failed\n");
            return false;
        }
    }
    ++g_windowCount;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, config.visible ? GLFW_TRUE : GLFW_FALSE);

    window_ = glfwCreateWindow(config.width, config.height, config.title.c_str(),
                               nullptr, nullptr);
    if (!window_) {
        std::fprintf(stderr, "[ember::platform] glfwCreateWindow failed "
                             "(OpenGL 4.5 core context unavailable?)\n");
        if (--g_windowCount == 0)
            glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(config.vsync ? 1 : 0);

    // Loading GL with the context already current is the ordering contract:
    // App.cpp relies on Window owning it (the Blender3D_Engine team's
    // gl3w bug was exactly this, out of order).
    if (!loadGLFunctions()) {
        std::fprintf(stderr, "[ember::platform] OpenGL 4.5 core unavailable\n");
        glfwDestroyWindow(window_);
        window_ = nullptr;
        if (--g_windowCount == 0)
            glfwTerminate();
        return false;
    }

    installAutoCloseHook();
    installSmokeResizeHook(0);   // bounded-run resize (see hook above)

    input().reset(); // fresh input state for a fresh window

    glfwSetKeyCallback(window_, keyCallback);
    glfwSetMouseButtonCallback(window_, mouseButtonCallback);
    glfwSetCursorPosCallback(window_, cursorPosCallback);
    glfwSetScrollCallback(window_, scrollCallback);
    // THE defect-1 fix: these did not exist before 2026-10-16, so the rect
    // stayed at its creation size forever (orbit broken after maximise).
    glfwSetWindowSizeCallback(window_, windowSizeCallback);
    glfwSetFramebufferSizeCallback(window_, framebufferSizeCallback);

    rectDirty_ = true;          // belt: (re)sync on the first pollEvents too
    syncViewportRect(window_);  // and right now, before the first frame

    g_window = this;
    return true;
}

void Window::destroy() {
    if (!window_)
        return;
    if (g_window == this)
        g_window = nullptr;
    glfwDestroyWindow(window_);
    window_ = nullptr;
    if (--g_windowCount == 0)
        glfwTerminate();
}

bool Window::shouldClose() const {
    return !window_ || glfwWindowShouldClose(window_);
}

void Window::requestClose() {
    if (window_)
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

void Window::pollEvents() {
    if (rectDirty_) {           // first frame after create: rect may be stale
        syncViewportRect(window_);
        rectDirty_ = false;
    }

    input().beginFrame();  // clear edge states, seed deltas
    glfwPollEvents();      // callbacks feed Input; size callbacks resync the rect

    // Self-check, every frame, AFTER this frame's callbacks have run: if the
    // rect still disagrees with the window size, a resize path bypassed the
    // callbacks — i.e. the defect-1 regression. Log loudly (stdout, so the
    // smoke run sees it) and self-heal; the assertion makes a human wiggle the
    // mouse unnecessary to find it. Custom rects set via Window::setViewportRect
    // carry no capture size and are never flagged.
    if (window_) {
        int winW = 0, winH = 0;
        glfwGetWindowSize(window_, &winW, &winH);
        if (input().rectIsStaleFor(winW, winH)) {
            const auto r = input().viewportRect();
            std::fprintf(stderr,
                         "[ember::platform] STALE viewport rect: captured %dx%d, "
                         "window now %dx%d — resyncing (defect-1 regression)\n",
                         r.windowWidth, r.windowHeight, winW, winH);
            syncViewportRect(window_);
        }

        // One line per size change with BOTH sizes the operator wants compared:
        // input rect (window coords) and framebuffer (renderer's resize space).
        // D17: rect == window size always; fb == window * DPI scale. If a future
        // change makes them disagree, the smoke log shows it without a mouse.
        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(window_, &fbW, &fbH);
        static int lastWinW = -1, lastWinH = -1, lastFbW = -1, lastFbH = -1;
        if (winW != lastWinW || winH != lastWinH || fbW != lastFbW || fbH != lastFbH) {
            lastWinW = winW; lastWinH = winH; lastFbW = fbW; lastFbH = fbH;
            const auto  r = input().viewportRect();
            const float scale = winW > 0 ? float(fbW) / float(winW) : 1.0f;
            std::printf("[ember::platform] size: window=%dx%d fb=%dx%d scale=%.2f "
                        "rect=%dx%d(captured %dx%d) %s\n",
                        winW, winH, fbW, fbH, scale, r.w, r.h,
                        r.windowWidth, r.windowHeight,
                        (r.w == winW && r.h == winH) ? "rect=window OK"
                                                     : "rect!=window MISMATCH");
        }
    }

    // F11: fullscreen toggle (WHAT-REMAINS defect 1, requirement 5 — no
    // in-app fullscreen existed before). Edges were just fed above.
    if (input().keyPressed(Key::F11))
        toggleFullscreen();

    // Smoke resize hook (see installSmokeResizeHook): resize once, mid-run, so
    // the bounded smoke actually crosses a size change and the callback path is
    // what the checker is watching.
    if (g_smokeResizeAtFrame >= 0 && ++g_pollFrames == g_smokeResizeAtFrame && window_) {
        glfwSetWindowSize(window_, g_smokeResizeW, g_smokeResizeH);
        g_smokeResizeAtFrame = -1;   // once only
    }

    input().endFrame();    // finalise mouse delta / scroll for this frame

    if (g_autoCloseAt > 0.0 && glfwGetTime() >= g_autoCloseAt) {
        std::printf("[ember::platform] auto-close: requesting window close\n");
        g_autoCloseAt = -1.0;
        requestClose();
    }
}

void Window::swapBuffers() {
    if (window_)
        glfwSwapBuffers(window_);
}

int Window::width() const {
    int w = 0, h = 0;
    if (window_)
        glfwGetFramebufferSize(window_, &w, &h);
    return w;
}

int Window::height() const {
    int w = 0, h = 0;
    if (window_)
        glfwGetFramebufferSize(window_, &w, &h);
    return h;
}

int Window::windowWidth() const {
    int w = 0, h = 0;
    if (window_)
        glfwGetWindowSize(window_, &w, &h);
    return w;
}

int Window::windowHeight() const {
    int w = 0, h = 0;
    if (window_)
        glfwGetWindowSize(window_, &w, &h);
    return h;
}

void* Window::nativeHandle() const { return window_; }

void Window::toggleFullscreen() {
    if (!window_)
        return;

    if (!fullscreen_) {
        // Remember where to come back to (GLFW has no "previous" state).
        glfwGetWindowPos(window_, &restoreX_, &restoreY_);
        glfwGetWindowSize(window_, &restoreW_, &restoreH_);
        GLFWmonitor* monitor = glfwGetWindowMonitor(window_);
        if (!monitor)
            monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (!mode)
            return;
        glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height,
                             mode->refreshRate);
        fullscreen_ = true;
        std::printf("[ember::platform] fullscreen on (%s) %dx%d@%d\n",
                    glfwGetMonitorName(monitor), mode->width, mode->height,
                    mode->refreshRate);
    } else {
        glfwSetWindowMonitor(window_, nullptr, restoreX_, restoreY_,
                             restoreW_, restoreH_, GLFW_DONT_CARE);
        fullscreen_ = false;
        std::printf("[ember::platform] back to windowed %dx%d at (%d,%d)\n",
                    restoreW_, restoreH_, restoreX_, restoreY_);
    }

    // The size callbacks fire on the next poll; resync now so the rect is
    // correct within this frame, not one frame late.
    syncViewportRect(window_);
}

bool Window::isFullscreen() const { return fullscreen_; }

void Window::setViewportRect(int x, int y, int w, int h) {
    input().setViewportRect(x, y, w, h);   // capture size left unknown (0)
}

// -- free functions --------------------------------------------------------

double timeSeconds() {
    // Monotonic since GLFW init; 0.0 if not initialised.
    return glfwGetTime();
}

} // namespace ember::platform
