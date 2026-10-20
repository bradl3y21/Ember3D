// Headless lifecycle checks for the editor's Cycles-style viewport controller.
#include "harness.h"

#include "core/Math.h"
#include "core/Types.h"
#include "editor/Editor.h"
#include "render/Renderer.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace ember;

#define CHECK_MSG(cond, ...)                                                       \
    do {                                                                          \
        ++harness::g_checks;                                                      \
        if (!(cond))                                                              \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

TEST(editor_cycles_viewport_debounces_and_does_not_restart_a_stable_packet) {
    editor::Editor editor;
    render::Renderer renderer;
    ViewportSettings viewport;
    viewport.shading = ShadingMode::Rendered;
    renderer.setViewportSettings(viewport);

    RenderPacket packet;
    packet.width = 4;
    packet.height = 4;
    packet.view = Mat4::identity();
    packet.projection = Mat4::identity();
    packet.cameraPosition = {0.0f, 0.0f, 3.0f};
    packet.renderSettings.engine = RenderEngine::Cycles;
    packet.renderSettings.samplesPerPixel = 128;
    packet.renderSettings.maxBounces = 0;

    viewport.shading = ShadingMode::Solid;
    renderer.setViewportSettings(viewport);
    editor.prepareViewportRender(packet, renderer, 0.9, true);
    CHECK(!editor.cyclesViewportProgress().visible);
    CHECK_EQ(editor.cyclesViewportProgress().generation, 0u);
    viewport.shading = ShadingMode::Rendered;
    renderer.setViewportSettings(viewport);

    editor.prepareViewportRender(packet, renderer, 1.0, true);
    CHECK(editor.cyclesViewportProgress().visible);
    CHECK(editor.cyclesViewportProgress().settling);
    CHECK_EQ(editor.cyclesViewportProgress().generation, 0u);

    editor.prepareViewportRender(packet, renderer, 1.19, true);
    CHECK_EQ(editor.cyclesViewportProgress().generation, 1u);
    const render::OfflineRenderState firstState = editor.cyclesViewportProgress().state;
    CHECK(firstState == render::OfflineRenderState::Running ||
          firstState == render::OfflineRenderState::Completed);
    CHECK_EQ(editor.cyclesViewportProgress().targetSamples, 128u);
    CHECK(editor.cyclesViewportProgress().samplesDone <= 128u);
    // A stable content/settings key must keep the same job generation.
    editor.prepareViewportRender(packet, renderer, 1.20, true);
    editor.prepareViewportRender(packet, renderer, 1.21, true);
    CHECK_EQ(editor.cyclesViewportProgress().generation, 1u);

    // Changing a render setting invalidates the prior snapshot. Simulated time
    // advances independently of scheduling so this test has no timing threshold.
    double now = 1.21;
    packet.renderSettings.seed += 1;
    now += 0.01;
    editor.prepareViewportRender(packet, renderer, now, true);
    CHECK(editor.cyclesViewportProgress().settling);
    for (int i = 0; i < 2000 && editor.cyclesViewportProgress().generation < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        now += 0.01;
        editor.prepareViewportRender(packet, renderer, now, true);
    }
    CHECK_EQ(editor.cyclesViewportProgress().generation, 2u);

    // Leaving Rendered retires the job even while the scene engine stays Cycles.
    viewport.shading = ShadingMode::Solid;
    renderer.setViewportSettings(viewport);
    now += 0.01;
    editor.prepareViewportRender(packet, renderer, now, true);
    CHECK(!editor.cyclesViewportProgress().visible);

    // Switching the scene engine to Raster is independently gated as well.
    viewport.shading = ShadingMode::Rendered;
    renderer.setViewportSettings(viewport);
    packet.renderSettings.engine = RenderEngine::Raster;
    now += 0.01;
    editor.prepareViewportRender(packet, renderer, now, true);
    CHECK(!editor.cyclesViewportProgress().visible);
}

TEST(editor_cycles_viewport_stops_when_the_viewport_area_disappears) {
    editor::Editor editor;
    render::Renderer renderer;
    ViewportSettings viewport;
    viewport.shading = ShadingMode::Rendered;
    renderer.setViewportSettings(viewport);

    RenderPacket packet;
    packet.width = 4;
    packet.height = 4;
    packet.view = Mat4::identity();
    packet.projection = Mat4::identity();
    packet.renderSettings.engine = RenderEngine::Cycles;
    packet.renderSettings.samplesPerPixel = 256;
    packet.renderSettings.maxBounces = 0;
    editor.prepareViewportRender(packet, renderer, 0.0, true);
    editor.prepareViewportRender(packet, renderer, 0.2, true);
    CHECK_EQ(editor.cyclesViewportProgress().generation, 1u);

    editor.prepareViewportRender(packet, renderer, 0.21, false);
    CHECK(!editor.cyclesViewportProgress().visible);
}

TEST(editor_cycles_viewport_restarts_for_camera_geometry_material_light_and_settings_changes) {
    editor::Editor editor;
    render::Renderer renderer;
    ViewportSettings viewport;
    viewport.shading = ShadingMode::Rendered;
    renderer.setViewportSettings(viewport);

    MeshData mesh;
    mesh.positions = {-1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    mesh.indices = {0, 1, 2};
    RenderPacket packet;
    packet.width = 4;
    packet.height = 4;
    packet.view = Mat4::identity();
    packet.projection = Mat4::identity();
    packet.cameraPosition = {0.0f, 0.0f, 3.0f};
    packet.renderSettings.engine = RenderEngine::Cycles;
    packet.renderSettings.samplesPerPixel = 256;
    packet.renderSettings.maxBounces = 0;
    DrawItem draw;
    draw.object = ObjectId{4};
    draw.mesh = &mesh;
    draw.material.baseColor = {0.4f, 0.7f, 0.2f, 1.0f};
    packet.items.push_back(draw);

    editor.prepareViewportRender(packet, renderer, 0.0, true);
    editor.prepareViewportRender(packet, renderer, 0.2, true);
    CHECK_EQ(editor.cyclesViewportProgress().generation, 1u);

    double now = 0.3;
    auto changeAndRestart = [&](auto&& change) {
        const std::uint64_t desired = editor.cyclesViewportProgress().generation + 1u;
        change();
        editor.prepareViewportRender(packet, renderer, now, true);
        CHECK(editor.cyclesViewportProgress().settling);
        CHECK(!editor.cyclesViewportProgress().hasPreview);
        for (int i = 0; i < 2000 && editor.cyclesViewportProgress().generation < desired; ++i) {
            now += 0.01;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            editor.prepareViewportRender(packet, renderer, now, true);
        }
        CHECK_EQ(editor.cyclesViewportProgress().generation, desired);
    };

    changeAndRestart([&] {
        packet.cameraPosition.x += 0.2f;
        packet.view = lookAt(packet.cameraPosition, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    });
    changeAndRestart([&] { packet.projection = perspective(radians(60.0f), 1.0f, 0.1f, 100.0f); });
    changeAndRestart([&] { ++packet.width; });
    changeAndRestart([&] { mesh.positions[0] -= 0.1f; });
    changeAndRestart([&] { packet.items[0].material.baseColor.g += 0.1f; });
    changeAndRestart([&] {
        LightInstance light;
        light.intensity = 2.0f;
        packet.lights.push_back(light);
    });
    changeAndRestart([&] { ++packet.renderSettings.seed; });
    changeAndRestart([&] { ++packet.renderSettings.maxBounces; });
    changeAndRestart([&] { ++packet.renderSettings.samplesPerPixel; });

    editor.shutdown();
}

TEST(editor_cycles_viewport_cancel_is_sticky_until_packet_changes_and_shutdown_joins) {
    editor::Editor editor;
    render::Renderer renderer;
    ViewportSettings viewport;
    viewport.shading = ShadingMode::Rendered;
    renderer.setViewportSettings(viewport);
    RenderPacket packet;
    packet.width = 64;
    packet.height = 48;
    packet.view = Mat4::identity();
    packet.projection = Mat4::identity();
    packet.renderSettings.engine = RenderEngine::Cycles;
    packet.renderSettings.samplesPerPixel = 100000;
    packet.renderSettings.maxBounces = 0;

    editor.prepareViewportRender(packet, renderer, 0.0, true);
    editor.prepareViewportRender(packet, renderer, 0.2, true);
    CHECK_EQ(editor.cyclesViewportProgress().generation, 1u);
    editor.cancelCyclesViewport();
    editor.prepareViewportRender(packet, renderer, 0.21, true);
    CHECK(editor.cyclesViewportProgress().state == render::OfflineRenderState::Cancelled);
    CHECK(!editor.cyclesViewportProgress().hasPreview);
    editor.prepareViewportRender(packet, renderer, 5.0, true);
    CHECK_EQ(editor.cyclesViewportProgress().generation, 1u);

    packet.renderSettings.seed += 1;
    editor.prepareViewportRender(packet, renderer, 5.1, true);
    CHECK(editor.cyclesViewportProgress().settling);
    for (int i = 0; i < 2000 && editor.cyclesViewportProgress().generation < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        editor.prepareViewportRender(packet, renderer, 5.3 + i * 0.01, true);
    }
    CHECK_EQ(editor.cyclesViewportProgress().generation, 2u);
    editor.shutdown();
    CHECK(!editor.cyclesViewportProgress().visible);
    CHECK(!editor.cyclesViewportProgress().workerRunning);
}

TEST(editor_cycles_viewport_compare_runs_cpu_preview_with_raster_engine_and_saves_completed_formats) {
    editor::Editor editor;
    render::Renderer renderer;
    ViewportSettings viewport;
    viewport.shading = ShadingMode::Solid;
    viewport.compareRasterCycles = true;
    renderer.setViewportSettings(viewport);

    RenderPacket packet;
    packet.width = 2;
    packet.height = 2;
    packet.view = Mat4::identity();
    packet.projection = Mat4::identity();
    packet.cameraPosition = {0.0f, 0.0f, 3.0f};
    packet.renderSettings.engine = RenderEngine::Raster;
    packet.renderSettings.samplesPerPixel = 1;
    packet.renderSettings.maxBounces = 0;

    editor.prepareViewportRender(packet, renderer, 0.0, true);
    CHECK(!editor.cyclesViewportProgress().visible);
    viewport.shading = ShadingMode::Rendered;
    renderer.setViewportSettings(viewport);
    editor.prepareViewportRender(packet, renderer, 0.01, true);
    CHECK(editor.cyclesViewportProgress().visible);
    CHECK(editor.cyclesViewportProgress().settling);
    editor.prepareViewportRender(packet, renderer, 0.2, true);
    CHECK_EQ(editor.cyclesViewportProgress().generation, 1u);
    CHECK(packet.renderSettings.engine == RenderEngine::Raster);

    double now = 0.21;
    for (int i = 0; i < 2000 &&
         (editor.cyclesViewportProgress().state != render::OfflineRenderState::Completed ||
          !editor.cyclesViewportProgress().hasPreview); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        now += 0.01;
        editor.prepareViewportRender(packet, renderer, now, true);
    }
    CHECK(editor.cyclesViewportProgress().state == render::OfflineRenderState::Completed);
    CHECK(editor.cyclesViewportProgress().hasPreview);

    const std::filesystem::path base = std::filesystem::temp_directory_path() /
                                       "ember3d-viewport-preview-workflow";
    const std::filesystem::path pfmPath = base.string() + ".pfm";
    const std::filesystem::path ppmPath = base.string() + ".ppm";
    std::error_code ec;
    std::filesystem::remove(pfmPath, ec);
    std::filesystem::remove(ppmPath, ec);

    CHECK(editor.saveCompletedViewportPreview(pfmPath, ember::editor::ViewportPreviewFormat::PFM));
    auto save = editor.viewportPreviewSaveProgress();
    for (int i = 0; i < 2000 && save.state == ember::editor::ViewportPreviewSaveState::Running; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        save = editor.viewportPreviewSaveProgress();
    }
    CHECK(save.state == ember::editor::ViewportPreviewSaveState::Completed);
    CHECK(save.path == pfmPath);
    {
        std::ifstream in(pfmPath, std::ios::binary);
        std::string magic(2, '\0');
        in.read(magic.data(), 2);
        CHECK(magic == "PF");
    }

    CHECK(editor.saveCompletedViewportPreview(ppmPath, ember::editor::ViewportPreviewFormat::PPM));
    save = editor.viewportPreviewSaveProgress();
    for (int i = 0; i < 2000 && save.state == ember::editor::ViewportPreviewSaveState::Running; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        save = editor.viewportPreviewSaveProgress();
    }
    CHECK(save.state == ember::editor::ViewportPreviewSaveState::Completed);
    CHECK(save.path == ppmPath);
    {
        std::ifstream in(ppmPath, std::ios::binary);
        std::string magic(2, '\0');
        in.read(magic.data(), 2);
        CHECK(magic == "P6");
    }

    editor.shutdown();
    std::filesystem::remove(pfmPath, ec);
    std::filesystem::remove(ppmPath, ec);
}

HARNESS_MAIN()
