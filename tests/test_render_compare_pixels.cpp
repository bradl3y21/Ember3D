// GPU pixel tests for the Rendered-shading Raster/Cycles viewport wipe.
#include "harness.h"

#include <glad/gl.h>

#include <algorithm>
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
        config.title = "ember-render-compare-pixels";
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
    GLint oldRead = 0, oldBuffer = GL_BACK;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldRead);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glGetIntegerv(GL_READ_BUFFER, &oldBuffer);
    glReadBuffer(GL_BACK);
    glReadPixels(x, kWindowHeight - 1 - yTop, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
    glReadBuffer(static_cast<GLenum>(oldBuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldRead));
    return pixel;
}

bool nearByte(ember::u8 value, int expected, int tolerance = 4) {
    return std::abs(static_cast<int>(value) - expected) <= tolerance;
}

int displayByte(float linear) {
    const float mapped = std::max(linear, 0.0f) / (1.0f + std::max(linear, 0.0f));
    const float srgb = mapped <= 0.0031308f ? 12.92f * mapped
        : 1.055f * std::pow(mapped, 1.0f / 2.4f) - 0.055f;
    return static_cast<int>(std::lround(srgb * 255.0f));
}

void setCompareSettings(ember::render::Renderer& renderer, float split) {
    ember::ViewportSettings viewport;
    viewport.shading = ember::ShadingMode::Rendered;
    viewport.compareRasterCycles = true;
    viewport.compareSplit = split;
    viewport.showGrid = false;
    viewport.showAxes = false;
    viewport.showGizmo = false;
    viewport.showOrigins = false;
    viewport.showSelectionOutline = false;
    viewport.shadows.enabled = false;
    renderer.setViewportSettings(viewport);
}

ember::RenderPacket packet() {
    ember::RenderPacket result;
    result.width = kViewportWidth;
    result.height = kViewportHeight;
    result.view = ember::Mat4::identity();
    result.projection = ember::Mat4::identity();
    // A distinctive raster color; compare mode must work even when the scene
    // engine selector remains Raster.
    result.background.color = {0.25f, 0.0f, 0.0f, 1.0f};
    result.background.strength = 1.0f;
    result.renderSettings.engine = ember::RenderEngine::Raster;
    return result;
}

ember::render::PathTraceImage cyclesImage() {
    ember::render::PathTraceImage image;
    image.width = 4;
    image.height = 2;
    image.samplesPerPixel = 1;
    image.linearPixels.assign(8, ember::Color{0.0f, 0.0f, 0.5f, 1.0f});
    return image;
}

} // namespace

TEST_GPU(rendered_compare_uses_raster_left_cycles_right_and_same_display_transform) {
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
    renderer.setViewportRect(kViewportRect, kViewportRect);
    setCompareSettings(renderer, 0.5f);
    ember::RenderPacket p = packet();
    ember::render::PathTraceImage image = cyclesImage();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.07f, 0.11f, 0.17f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const Pixel outsideBefore = readWindowPixel(4, 4);

    renderer.setPathTraceImage(&image);
    renderer.renderFrame(p);
    std::vector<ember::u8> rgba;
    renderer.readViewport(0, 0, kViewportWidth, kViewportHeight, rgba);
    CHECK_EQ(rgba.size(), static_cast<std::size_t>(kViewportWidth * kViewportHeight * 4));
    if (rgba.size() == static_cast<std::size_t>(kViewportWidth * kViewportHeight * 4)) {
        const Pixel raster = at(rgba, kViewportWidth, 8, 24);
        const Pixel cycles = at(rgba, kViewportWidth, 56, 24);
        CHECK(nearByte(raster.r, displayByte(0.25f)) && raster.g < 4 && raster.b < 4);
        CHECK(cycles.r < 4 && cycles.g < 4 && nearByte(cycles.b, displayByte(0.5f)));
    }
    const Pixel outsideAfter = readWindowPixel(4, 4);
    CHECK_EQ(outsideAfter.r, outsideBefore.r);
    CHECK_EQ(outsideAfter.g, outsideBefore.g);
    CHECK_EQ(outsideAfter.b, outsideBefore.b);
    renderer.shutdown();
}

TEST_GPU(rendered_compare_clamps_split_and_missing_preview_falls_back_to_full_raster) {
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
    renderer.setViewportRect(kViewportRect, kViewportRect);
    ember::RenderPacket p = packet();
    ember::render::PathTraceImage image = cyclesImage();

    setCompareSettings(renderer, -10.0f);
    renderer.setPathTraceImage(&image);
    renderer.renderFrame(p);
    std::vector<ember::u8> rgba;
    renderer.readViewport(0, 0, kViewportWidth, kViewportHeight, rgba);
    CHECK(nearByte(at(rgba, kViewportWidth, 2, 24).r, displayByte(0.25f)));
    CHECK(nearByte(at(rgba, kViewportWidth, 3, 24).b, displayByte(0.5f)));
    CHECK(nearByte(at(rgba, kViewportWidth, 4, 24).b, displayByte(0.5f)));

    setCompareSettings(renderer, 4.0f);
    renderer.setPathTraceImage(&image);
    renderer.renderFrame(p);
    renderer.readViewport(0, 0, kViewportWidth, kViewportHeight, rgba);
    CHECK(nearByte(at(rgba, kViewportWidth, 59, 24).r, displayByte(0.25f)));
    CHECK(nearByte(at(rgba, kViewportWidth, 60, 24).r, displayByte(0.25f)));
    CHECK(nearByte(at(rgba, kViewportWidth, 61, 24).b, displayByte(0.5f)));

    // No borrowed preview this frame: do not reuse the texture from the prior
    // frame, and show raster over the whole viewport while the editor reports
    // its warming-up status.
    setCompareSettings(renderer, 0.5f);
    renderer.setPathTraceImage(nullptr);
    renderer.renderFrame(p);
    renderer.readViewport(0, 0, kViewportWidth, kViewportHeight, rgba);
    const Pixel left = at(rgba, kViewportWidth, 8, 24);
    const Pixel right = at(rgba, kViewportWidth, 56, 24);
    CHECK(nearByte(left.r, displayByte(0.25f)));
    CHECK(nearByte(right.r, displayByte(0.25f)));
    CHECK(right.b < 4);
    renderer.shutdown();
}

TEST_GPU(compare_off_preserves_scene_engine_selection_behavior) {
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
    renderer.setViewportRect(kViewportRect, kViewportRect);
    ember::ViewportSettings viewport;
    viewport.shading = ember::ShadingMode::Rendered;
    viewport.compareRasterCycles = false;
    viewport.showGrid = viewport.showAxes = viewport.showGizmo = false;
    viewport.showOrigins = viewport.showSelectionOutline = false;
    viewport.shadows.enabled = false;
    renderer.setViewportSettings(viewport);
    ember::RenderPacket p = packet();
    ember::render::PathTraceImage image = cyclesImage();

    // Raster selected: a borrowed path image is ignored.
    p.renderSettings.engine = ember::RenderEngine::Raster;
    renderer.setPathTraceImage(&image);
    renderer.renderFrame(p);
    std::vector<ember::u8> raster;
    renderer.readViewport(0, 0, kViewportWidth, kViewportHeight, raster);
    CHECK(nearByte(at(raster, kViewportWidth, 56, 24).r, displayByte(0.25f)));
    CHECK(at(raster, kViewportWidth, 56, 24).b < 4);

    // Cycles selected: the same preview continues to replace raster full-frame.
    p.renderSettings.engine = ember::RenderEngine::Cycles;
    renderer.setPathTraceImage(&image);
    renderer.renderFrame(p);
    std::vector<ember::u8> cycles;
    renderer.readViewport(0, 0, kViewportWidth, kViewportHeight, cycles);
    CHECK(at(cycles, kViewportWidth, 8, 24).r < 4);
    CHECK(nearByte(at(cycles, kViewportWidth, 8, 24).b, displayByte(0.5f)));
    renderer.shutdown();
}

TEST_GPU(compare_wipe_maps_the_complete_cycles_image_into_the_right_half) {
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
    renderer.setViewportRect(kViewportRect, kViewportRect);
    setCompareSettings(renderer, 0.5f);
    ember::RenderPacket p = packet();
    ember::render::PathTraceImage image;
    image.width = 4;
    image.height = 1;
    image.samplesPerPixel = 1;
    image.linearPixels = {{0.5f, 0.0f, 0.0f, 1.0f},
                          {0.5f, 0.0f, 0.0f, 1.0f},
                          {0.0f, 0.5f, 0.0f, 1.0f},
                          {0.0f, 0.5f, 0.0f, 1.0f}};
    renderer.setPathTraceImage(&image);
    renderer.renderFrame(p);
    std::vector<ember::u8> rgba;
    renderer.readViewport(0, 0, kViewportWidth, kViewportHeight, rgba);
    const Pixel cyclesLeft = at(rgba, kViewportWidth, 36, 24);
    const Pixel cyclesRight = at(rgba, kViewportWidth, 60, 24);
    CHECK(cyclesLeft.r > cyclesLeft.g * 2 && nearByte(cyclesLeft.r, displayByte(0.5f)));
    CHECK(cyclesRight.g > cyclesRight.r * 2 && nearByte(cyclesRight.g, displayByte(0.5f)));
    renderer.shutdown();
}

HARNESS_MAIN()
