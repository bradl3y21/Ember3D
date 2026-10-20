// GPU pixel proof for presenting linear PathTraceImage data in the viewport.
#include "harness.h"

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "core/Types.h"
#include "platform/Window.h"
#include "render/Renderer.h"

namespace {

#define CHECK_MSG(cond, ...)                                                       \
    do {                                                                          \
        ++harness::g_checks;                                                      \
        if (!(cond))                                                              \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

constexpr int kWindowWidth = 96;
constexpr int kWindowHeight = 72;
constexpr int kViewportWidth = 64;
constexpr int kViewportHeight = 48;
constexpr ember::Rect kViewportRect{16, 8, kViewportWidth, kViewportHeight};

bool ensureContext() {
    static ember::platform::Window* window = [] {
        auto* result = new ember::platform::Window();
        ember::platform::WindowConfig config;
        config.width = kWindowWidth;
        config.height = kWindowHeight;
        config.visible = false;
        config.title = "ember-cycles-viewport-pixels";
        if (!result->create(config)) {
            delete result;
            return static_cast<ember::platform::Window*>(nullptr);
        }
        return result;
    }();
    return window != nullptr;
}

struct Pixel { ember::u8 r = 0, g = 0, b = 0, a = 0; };

Pixel at(const std::vector<ember::u8>& rgba, int width, int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

Pixel readWindowPixel(int x, int yTop) {
    Pixel pixel;
    GLint oldRead = 0;
    GLint oldBuffer = GL_BACK;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldRead);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glGetIntegerv(GL_READ_BUFFER, &oldBuffer);
    glReadBuffer(GL_BACK);
    glReadPixels(x, kWindowHeight - 1 - yTop, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
    glReadBuffer(static_cast<GLenum>(oldBuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldRead));
    return pixel;
}

bool nearByte(ember::u8 value, int expected, int tolerance = 5) {
    return std::abs(static_cast<int>(value) - expected) <= tolerance;
}

} // namespace

TEST_GPU(cycles_preview_scales_flips_and_tone_maps_inside_the_viewport_only) {
    if (!ensureContext()) {
        CHECK_MSG(false, "hidden OpenGL context unavailable");
        return;
    }

    ember::render::Renderer renderer;
    if (!renderer.init()) {
        CHECK_MSG(false, "renderer initialization failed");
        return;
    }
    renderer.resize(kWindowWidth, kWindowHeight);
    renderer.setViewportRect(kViewportRect, ember::Rect{0, 0, kWindowWidth, kWindowHeight});
    renderer.setExposureStops(0.0f);

    ember::ViewportSettings viewport;
    viewport.shading = ember::ShadingMode::Rendered;
    viewport.showGrid = false;
    viewport.showAxes = false;
    viewport.showGizmo = false;
    viewport.showOrigins = false;
    viewport.showSelectionOutline = false;
    viewport.shadows.enabled = false;
    renderer.setViewportSettings(viewport);

    ember::RenderPacket packet;
    packet.width = kViewportWidth;
    packet.height = kViewportHeight;
    packet.view = ember::Mat4::identity();
    packet.projection = ember::Mat4::identity();
    packet.renderSettings.engine = ember::RenderEngine::Cycles;
    packet.background.color = {0.02f, 0.03f, 0.04f, 1.0f};
    packet.background.strength = 1.0f;

    ember::render::PathTraceImage image;
    image.width = 2;
    image.height = 2;
    image.samplesPerPixel = 1;
    image.linearPixels = {
        {4.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 2.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
    };

    // Capture one outside pixel from the normal fallback, then ensure a Cycles
    // upload does not paint outside the owning viewport Area.
    renderer.setPathTraceImage(nullptr);
    renderer.renderFrame(packet);
    const Pixel outsideBefore = readWindowPixel(4, 4);

    renderer.setPathTraceImage(&image);
    renderer.renderFrame(packet);
    std::vector<ember::u8> rgba;
    renderer.readViewport(0, 0, kViewportWidth, kViewportHeight, rgba);
    CHECK_EQ(rgba.size(), static_cast<std::size_t>(kViewportWidth * kViewportHeight * 4));

    const Pixel topLeft = at(rgba, kViewportWidth, 8, 8);
    const Pixel topRight = at(rgba, kViewportWidth, 56, 8);
    const Pixel bottomLeft = at(rgba, kViewportWidth, 8, 40);
    const Pixel bottomRight = at(rgba, kViewportWidth, 56, 40);
    CHECK(nearByte(topLeft.r, 231) && topLeft.g < 8 && topLeft.b < 8);
    CHECK(nearByte(topRight.g, 213) && topRight.r < 8 && topRight.b < 8);
    CHECK(nearByte(bottomLeft.b, 188) && bottomLeft.r < 8 && bottomLeft.g < 8);
    CHECK(nearByte(bottomRight.r, 188) && nearByte(bottomRight.g, 188) &&
          nearByte(bottomRight.b, 188));

    const Pixel outsideAfter = readWindowPixel(4, 4);
    CHECK_EQ(outsideAfter.r, outsideBefore.r);
    CHECK_EQ(outsideAfter.g, outsideBefore.g);
    CHECK_EQ(outsideAfter.b, outsideBefore.b);

    // Raster mode must ignore a borrowed path image rather than display stale data.
    packet.renderSettings.engine = ember::RenderEngine::Raster;
    renderer.renderFrame(packet);
    std::vector<ember::u8> rasterWithBorrowedImage;
    renderer.readViewport(0, 0, kViewportWidth, kViewportHeight, rasterWithBorrowedImage);
    renderer.setPathTraceImage(nullptr);
    renderer.renderFrame(packet);
    std::vector<ember::u8> rasterWithoutImage;
    renderer.readViewport(0, 0, kViewportWidth, kViewportHeight, rasterWithoutImage);
    CHECK(rasterWithBorrowedImage == rasterWithoutImage);

    renderer.shutdown();
}

HARNESS_MAIN()
