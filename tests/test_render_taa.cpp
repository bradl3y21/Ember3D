// Regression guard for the deterministic single-frame raster fallback.
// Temporal AA is intentionally not enabled: Renderer has no scene revision
// contract, so reprojected history could not be invalidated safely on edits.
#include "harness.h"

#include <glad/gl.h>

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

constexpr int kWidth = 64;
constexpr int kHeight = 48;

bool ensureContext() {
    static ember::platform::Window* window = [] {
        auto* result = new ember::platform::Window();
        ember::platform::WindowConfig config;
        config.width = kWidth;
        config.height = kHeight;
        config.visible = false;
        config.title = "ember-raster-taa-fallback";
        if (!result->create(config)) {
            delete result;
            return static_cast<ember::platform::Window*>(nullptr);
        }
        return result;
    }();
    return window != nullptr;
}

} // namespace

TEST_GPU(raster_single_frame_fallback_is_deterministic_and_uses_current_packet) {
    if (!ensureContext()) {
        CHECK_MSG(false, "hidden OpenGL context unavailable");
        return;
    }
    ember::render::Renderer renderer;
    if (!renderer.init()) {
        CHECK_MSG(false, "renderer initialization failed");
        return;
    }
    renderer.resize(kWidth, kHeight);
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
    packet.width = kWidth;
    packet.height = kHeight;
    packet.view = ember::Mat4::identity();
    packet.projection = ember::Mat4::identity();
    packet.background.color = {0.2f, 0.03f, 0.01f, 1.0f};
    packet.background.strength = 1.0f;
    packet.renderSettings.engine = ember::RenderEngine::Raster;

    renderer.renderFrame(packet);
    std::vector<ember::u8> first;
    renderer.readViewport(0, 0, kWidth, kHeight, first);
    renderer.renderFrame(packet);
    std::vector<ember::u8> repeated;
    renderer.readViewport(0, 0, kWidth, kHeight, repeated);
    CHECK(first == repeated);

    // A changed packet must replace the previous frame immediately. This tests
    // the history-free fallback, not temporal accumulation or reprojection.
    packet.background.color = {0.01f, 0.2f, 0.03f, 1.0f};
    renderer.renderFrame(packet);
    std::vector<ember::u8> changed;
    renderer.readViewport(0, 0, kWidth, kHeight, changed);
    CHECK(changed.size() == first.size());
    CHECK(changed != first);
    renderer.shutdown();
}

HARNESS_MAIN()
