// Raster image-texture/world-image pixel checks. These require a live GL
// context and are registered by the GPU-test CMake lane.
#include "harness.h"

#include <glad/gl.h>

#include "core/Types.h"
#include "platform/Window.h"
#include "render/Renderer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ember;

namespace {

#define CHECK_MSG(cond, ...)                                                       \
    do {                                                                          \
        ++harness::g_checks;                                                      \
        if (!(cond))                                                              \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

constexpr int kWidth = 320;
constexpr int kHeight = 240;

bool ensureContext() {
    static platform::Window* window = [] {
        auto* result = new platform::Window();
        platform::WindowConfig config;
        config.width = kWidth;
        config.height = kHeight;
        config.visible = false;
        config.title = "ember-raster-texture-pixels";
        if (!result->create(config)) {
            delete result;
            return static_cast<platform::Window*>(nullptr);
        }
        return result;
    }();
    return window != nullptr;
}

std::filesystem::path testDirectory(const char* suffix) {
    const auto path = std::filesystem::temp_directory_path() /
                      (std::string("ember-raster-texture-") + suffix);
    std::filesystem::create_directories(path);
    return path;
}

bool writePpm(const std::filesystem::path& path, const char* pixels) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "P3\n" << pixels;
    return out.good();
}

struct LinearRgb { float r = 0.0f, g = 0.0f, b = 0.0f; };

float srgbToLinear(float value) {
    return value <= 0.04045f ? value / 12.92f
                             : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

LinearRgb displayPixelToLinear(const std::vector<u8>& rgba, int width, int x, int y) {
    if (rgba.size() < static_cast<std::size_t>(width) * (y + 1) * 4u) return {};
    const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4u;
    const auto inverseReinhard = [](float value) {
        value = std::min(value, 0.999f);
        return value / (1.0f - value);
    };
    return {inverseReinhard(srgbToLinear(rgba[index] / 255.0f)),
            inverseReinhard(srgbToLinear(rgba[index + 1] / 255.0f)),
            inverseReinhard(srgbToLinear(rgba[index + 2] / 255.0f))};
}

struct RenderFixture {
    render::Renderer renderer;
    RenderPacket packet;
    bool ready = false;

    bool begin(int width = kWidth, int height = kHeight) {
        if (!ensureContext() || !renderer.init()) return false;
        renderer.resize(kWidth, kHeight);
        renderer.camera().setView(render::StandardView::Front);
        packet.width = width;
        packet.height = height;
        packet.view = renderer.camera().view();
        packet.projection = renderer.camera().projection(static_cast<float>(width) / height);
        packet.cameraPosition = renderer.camera().position();
        packet.background.color = {0.01f, 0.02f, 0.03f, 1.0f};
        packet.background.strength = 1.0f;
        packet.background.lighting = false;
        ViewportSettings settings;
        settings.shading = ShadingMode::Material;
        settings.showGrid = false;
        settings.showAxes = false;
        settings.showGizmo = false;
        settings.showOrigins = false;
        settings.showSelectionOutline = false;
        settings.shadows.enabled = false;
        renderer.setViewportSettings(settings);
        ready = true;
        return true;
    }

    std::vector<u8> renderPixels() {
        renderer.renderFrame(packet);
        std::vector<u8> pixels;
        renderer.readViewport(0, 0, packet.width, packet.height, pixels);
        return pixels;
    }
};

MeshData twoUvTriangles(float leftU, float rightU) {
    MeshData mesh;
    mesh.name = "raster_texture_uv_test";
    mesh.positions = {
        -2.0f, -1.0f, 0.0f,  0.0f, -1.0f, 0.0f, -1.0f, 1.0f, 0.0f,
         0.0f, -1.0f, 0.0f,  2.0f, -1.0f, 0.0f,  1.0f, 1.0f, 0.0f
    };
    mesh.normals = {0,0,1, 0,0,1, 0,0,1, 0,0,1, 0,0,1, 0,0,1};
    mesh.uvs = {leftU,0.5f, leftU,0.5f, leftU,0.5f,
                rightU,0.5f, rightU,0.5f, rightU,0.5f};
    mesh.indices = {0,1,2, 3,4,5};
    return mesh;
}

MaterialDesc whiteTextured(const std::string& path, ImageColorSpace colorSpace) {
    MaterialDesc material;
    material.baseColor = {1,1,1,1};
    material.baseColorTexture = {path, colorSpace};
    material.metallic = 1.0f;
    material.roughness = 0.8f;
    return material;
}

} // namespace

TEST_GPU(raster_base_color_images_sample_uvs_and_follow_material_slot_mapping) {
    const auto directory = testDirectory("uv-slots");
    CHECK(writePpm(directory / "uv.ppm", "2 1\n255\n255 0 0  0 255 0\n"));
    CHECK(writePpm(directory / "red.ppm", "1 1\n255\n255 0 0\n"));
    CHECK(writePpm(directory / "green.ppm", "1 1\n255\n0 255 0\n"));

    RenderFixture fixture;
    CHECK(fixture.begin());
    if (!fixture.ready) return;
    fixture.packet.assetBaseDirectory = directory.string();
    MeshData uvMesh = twoUvTriangles(0.25f, 0.75f);
    DrawItem item;
    item.mesh = &uvMesh;
    item.material = whiteTextured("uv.ppm", ImageColorSpace::Linear);
    fixture.packet.items = {item};
    const auto uvPixels = fixture.renderPixels();
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
    CHECK(fixture.renderer.camera().projectScreen({-1.0f, -0.35f, 0.0f},
          fixture.packet.width, fixture.packet.height, lx, ly));
    CHECK(fixture.renderer.camera().projectScreen({1.0f, -0.35f, 0.0f},
          fixture.packet.width, fixture.packet.height, rx, ry));
    const LinearRgb uvLeft = displayPixelToLinear(uvPixels, fixture.packet.width,
                                                   static_cast<int>(std::lround(lx)),
                                                   static_cast<int>(std::lround(ly)));
    const LinearRgb uvRight = displayPixelToLinear(uvPixels, fixture.packet.width,
                                                    static_cast<int>(std::lround(rx)),
                                                    static_cast<int>(std::lround(ry)));
    CHECK(uvLeft.r > uvLeft.g * 2.0f + 0.05f);
    CHECK(uvRight.g > uvRight.r * 2.0f + 0.05f);

    ViewportSettings settings = fixture.renderer.viewportSettings();
    settings.shading = ShadingMode::Rendered;
    fixture.renderer.setViewportSettings(settings);
    const auto renderedUvPixels = fixture.renderPixels();
    const LinearRgb renderedUvLeft = displayPixelToLinear(
        renderedUvPixels, fixture.packet.width, static_cast<int>(std::lround(lx)),
        static_cast<int>(std::lround(ly)));
    const LinearRgb renderedUvRight = displayPixelToLinear(
        renderedUvPixels, fixture.packet.width, static_cast<int>(std::lround(rx)),
        static_cast<int>(std::lround(ry)));
    CHECK(renderedUvLeft.r > renderedUvLeft.g * 2.0f + 0.05f);
    CHECK(renderedUvRight.g > renderedUvRight.r * 2.0f + 0.05f);

    MaterialSlot slots[2];
    slots[0].name = "red image";
    slots[0].material = whiteTextured("red.ppm", ImageColorSpace::SRgb);
    slots[1].name = "green image";
    slots[1].material = whiteTextured("green.ppm", ImageColorSpace::SRgb);
    const u32 mapping[2] = {0, 1};
    fixture.packet.items[0].materials = slots;
    fixture.packet.items[0].materialCount = 2;
    fixture.packet.items[0].triangleMaterialSlots = mapping;
    fixture.packet.items[0].triangleMaterialSlotCount = 2;
    fixture.packet.items[0].mesh = &uvMesh;
    // Slot assignment must take precedence over the item's legacy scalar and
    // is evaluated independently for each triangle of one uploaded mesh.
    const auto slotPixels = fixture.renderPixels();
    const LinearRgb slotLeft = displayPixelToLinear(slotPixels, fixture.packet.width,
                                                     static_cast<int>(std::lround(lx)),
                                                     static_cast<int>(std::lround(ly)));
    const LinearRgb slotRight = displayPixelToLinear(slotPixels, fixture.packet.width,
                                                      static_cast<int>(std::lround(rx)),
                                                      static_cast<int>(std::lround(ry)));
    CHECK(slotLeft.r > slotLeft.g * 2.0f + 0.05f);
    CHECK(slotRight.g > slotRight.r * 2.0f + 0.05f);
    fixture.renderer.shutdown();
    std::filesystem::remove_all(directory);
}

TEST_GPU(raster_base_color_image_srgb_and_linear_tags_change_linear_light_pixels) {
    const auto directory = testDirectory("colorspace");
    CHECK(writePpm(directory / "gray.ppm", "1 1\n255\n128 128 128\n"));

    RenderFixture fixture;
    CHECK(fixture.begin());
    if (!fixture.ready) return;
    fixture.packet.assetBaseDirectory = directory.string();
    MeshData mesh = twoUvTriangles(0.5f, 0.5f);
    DrawItem item;
    item.mesh = &mesh;
    item.material = whiteTextured("gray.ppm", ImageColorSpace::SRgb);
    item.material.metallic = 0.0f;
    fixture.packet.items = {item};
    LightInstance key;
    key.type = LightType::Directional;
    key.direction = {0.0f, 0.0f, -1.0f}; // light travels down onto the +Z-facing test surface
    key.color = {1.0f, 1.0f, 1.0f, 1.0f};
    key.intensity = 3.0f;
    fixture.packet.lights = {key};
    float sampleX = 0.0f, sampleY = 0.0f;
    CHECK(fixture.renderer.camera().projectScreen({-1.0f, -0.35f, 0.0f},
          fixture.packet.width, fixture.packet.height, sampleX, sampleY));
    const int x = static_cast<int>(std::lround(sampleX));
    const int y = static_cast<int>(std::lround(sampleY));
    for (const ShadingMode mode : {ShadingMode::Material, ShadingMode::Rendered}) {
        ViewportSettings settings = fixture.renderer.viewportSettings();
        settings.shading = mode;
        fixture.renderer.setViewportSettings(settings);
        fixture.packet.items[0].material.baseColorTexture.colorSpace = ImageColorSpace::SRgb;
        const auto srgbPixels = fixture.renderPixels();
        fixture.packet.items[0].material.baseColorTexture.colorSpace = ImageColorSpace::Linear;
        const auto linearPixels = fixture.renderPixels();
        const LinearRgb srgb = displayPixelToLinear(srgbPixels, fixture.packet.width,
                                                     x, y);
        const LinearRgb linear = displayPixelToLinear(linearPixels, fixture.packet.width,
                                                        x, y);
        CHECK(srgb.r > 0.0f && linear.r > 0.0f);
        CHECK_MSG(linear.r > srgb.r * 1.35f,
                  "color-space conversion too small: linear %.4f vs sRGB %.4f",
                  linear.r, srgb.r);
    }
    fixture.renderer.shutdown();
    std::filesystem::remove_all(directory);
}

TEST_GPU(rendered_environment_image_is_a_view_dependent_world_background) {
    const auto directory = testDirectory("environment");
    CHECK(writePpm(directory / "world.ppm", "2 1\n255\n255 0 0  0 255 0\n"));

    RenderFixture fixture;
    CHECK(fixture.begin());
    if (!fixture.ready) return;
    fixture.packet.assetBaseDirectory = directory.string();
    fixture.packet.items.clear();
    fixture.packet.background.environmentTexture = {"world.ppm", ImageColorSpace::SRgb};
    fixture.packet.background.lighting = false; // visible even when ambient lighting is disabled
    ViewportSettings settings = fixture.renderer.viewportSettings();
    settings.shading = ShadingMode::Rendered;
    fixture.renderer.setViewportSettings(settings);
    const auto pixels = fixture.renderPixels();
    const LinearRgb center = displayPixelToLinear(pixels, fixture.packet.width,
                                                   fixture.packet.width / 2,
                                                   fixture.packet.height / 2);
    CHECK(center.r > center.g * 2.0f + 0.1f);
    CHECK(fixture.renderer.lastStats().drawCalls >= 2); // env resolve + viewport display resolve

    MeshData surface = twoUvTriangles(0.5f, 0.5f);
    DrawItem object;
    object.mesh = &surface;
    object.material.baseColor = {1,1,1,1};
    object.material.metallic = 1.0f;
    object.material.roughness = 0.2f;
    fixture.packet.items = {object};
    fixture.packet.background.lighting = true;
    const auto withImageLighting = fixture.renderPixels();
    fixture.packet.background.environmentTexture = {};
    const auto flatWorldLighting = fixture.renderPixels();
    const LinearRgb envLit = displayPixelToLinear(withImageLighting, fixture.packet.width,
                                                   fixture.packet.width / 2 - 20,
                                                   fixture.packet.height / 2);
    const LinearRgb flatLit = displayPixelToLinear(flatWorldLighting, fixture.packet.width,
                                                    fixture.packet.width / 2 - 20,
                                                    fixture.packet.height / 2);
    CHECK(std::fabs(envLit.r - flatLit.r) + std::fabs(envLit.g - flatLit.g) +
          std::fabs(envLit.b - flatLit.b) > 0.1f);
    fixture.renderer.shutdown();
    std::filesystem::remove_all(directory);
}

TEST_GPU(environment_background_does_not_repaint_outside_the_viewport_scissor) {
    const auto directory = testDirectory("viewport-scissor");
    CHECK(writePpm(directory / "world.ppm", "1 1\n255\n255 0 255\n"));
    if (!ensureContext()) { CHECK(false); return; }

    // A sentinel outside the 3D rect must survive both the environment pass
    // and the display resolve. There is no owning-area clear rect in this case.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, kWidth, kHeight);
    glClearColor(0.07f, 0.16f, 0.73f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glFinish();
    u8 before[4]{};
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, before);

    RenderFixture fixture;
    CHECK(fixture.begin(144, 96));
    if (!fixture.ready) return;
    fixture.packet.assetBaseDirectory = directory.string();
    fixture.packet.background.environmentTexture = {"world.ppm", ImageColorSpace::SRgb};
    fixture.packet.background.lighting = false;
    ViewportSettings settings = fixture.renderer.viewportSettings();
    settings.shading = ShadingMode::Rendered;
    fixture.renderer.setViewportSettings(settings);
    fixture.renderer.setViewportRect(Rect{40, 32, 144, 96});
    fixture.renderer.renderFrame(fixture.packet);
    glFinish();
    u8 after[4]{};
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, after);
    CHECK(before[0] == after[0] && before[1] == after[1] &&
          before[2] == after[2] && before[3] == after[3]);
    fixture.renderer.shutdown();
    std::filesystem::remove_all(directory);
}

HARNESS_MAIN()
