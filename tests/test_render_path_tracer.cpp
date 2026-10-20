// CPU-only regression coverage for the separate progressive PathTracer.
// No scene module or OpenGL context is needed: the fixture builds the exact
// core RenderPacket consumed by PathTracer::setScene().
#include "harness.h"

#include "core/Math.h"
#include "core/Types.h"
#include "render/PathTracer.h"
#include "render/PathSampling.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ember;
using ember::render::PathTraceImage;
using ember::render::PathTraceSettings;
using ember::render::PathTracer;
using ember::render::PathAdaptiveSamplingSettings;

namespace {

constexpr int kWidth = 24;
constexpr int kHeight = 16;

void makeQuad(MeshData& mesh, bool verticalWall) {
    if (!verticalWall) {
        // Large +Z-facing diffuse floor, covering every camera ray.
        mesh.positions = {
            -3.0f, -3.0f, 0.0f,
             3.0f, -3.0f, 0.0f,
             3.0f,  3.0f, 0.0f,
            -3.0f,  3.0f, 0.0f,
        };
        mesh.normals = {
            0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
            0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
        };
        mesh.indices = {0, 1, 2, 0, 2, 3};
    } else {
        // A diffuse wall outside the camera frustum but inside the floor's
        // upper hemisphere. Some indirect rays hit it and some miss, giving
        // the environment-lit image a real, bounded Monte Carlo variance.
        mesh.positions = {
            2.0f, -5.0f, -2.0f,
            2.0f,  5.0f,  4.0f,
            2.0f,  5.0f, -2.0f,
            2.0f, -5.0f,  4.0f,
        };
        mesh.normals = {
            -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f,
            -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f,
        };
        mesh.indices = {0, 1, 2, 0, 3, 1};
    }
}

void setCamera(RenderPacket& packet) {
    packet.cameraPosition = {0.0f, 0.0f, 3.0f};
    packet.view = lookAt(packet.cameraPosition, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    packet.projection = perspective(radians(35.0f), 1.0f, 0.1f, 100.0f);
    packet.background.color = {0.0f, 0.0f, 0.0f, 1.0f};
    packet.background.strength = 0.0f;
    packet.background.lighting = false;
}

struct EmissionBounceScene {
    std::array<MeshData, 2> meshes;
    RenderPacket packet;

    EmissionBounceScene() {
        makeQuad(meshes[0], false);
        // The vertical emitter is outside the primary camera frustum, but the
        // +Z cosine hemisphere from the floor reaches its x=2 plane.
        meshes[1].positions = {
            2.0f, -100.0f, -100.0f,
            2.0f,  100.0f,  100.0f,
            2.0f,  100.0f, -100.0f,
            2.0f, -100.0f,  100.0f,
        };
        meshes[1].normals = {
            -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f,
            -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f,
        };
        meshes[1].indices = {0, 1, 2, 0, 3, 1};

        DrawItem floor;
        floor.mesh = &meshes[0];
        floor.material.baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
        floor.model = Mat4::identity();
        packet.items.push_back(floor);

        DrawItem emitter;
        emitter.mesh = &meshes[1];
        emitter.material.baseColor = {0.0f, 0.0f, 0.0f, 1.0f};
        emitter.material.emissionColor = {0.4f, 0.2f, 0.1f, 1.0f};
        emitter.material.emissionStrength = 5.0f;
        emitter.model = Mat4::identity();
        packet.items.push_back(emitter);
        setCamera(packet);
        packet.lights.clear();
    }
};

struct LargeTiledEmissionScene {
    MeshData mesh;
    RenderPacket packet;

    explicit LargeTiledEmissionScene(int side) {
        constexpr float extent = 4.0f;
        const float step = (2.0f * extent) / static_cast<float>(side);
        mesh.positions.reserve(static_cast<std::size_t>(side) * side * 12);
        mesh.normals.reserve(static_cast<std::size_t>(side) * side * 12);
        mesh.indices.reserve(static_cast<std::size_t>(side) * side * 6);
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const float x0 = -extent + x * step;
                const float x1 = x0 + step;
                const float y0 = -extent + y * step;
                const float y1 = y0 + step;
                const u32 first = static_cast<u32>(mesh.positions.size() / 3);
                const std::array<Vec3, 4> points{{
                    {x0, y0, 0.0f}, {x1, y0, 0.0f},
                    {x1, y1, 0.0f}, {x0, y1, 0.0f},
                }};
                for (Vec3 point : points) {
                    mesh.positions.insert(mesh.positions.end(), {point.x, point.y, point.z});
                    mesh.normals.insert(mesh.normals.end(), {0.0f, 0.0f, 1.0f});
                }
                mesh.indices.insert(mesh.indices.end(), {
                    first, first + 1, first + 2, first, first + 2, first + 3,
                });
            }
        }
        DrawItem surface;
        surface.mesh = &mesh;
        surface.material.baseColor = {0.0f, 0.0f, 0.0f, 1.0f};
        surface.material.emissionColor = {0.25f, 0.5f, 0.75f, 1.0f};
        surface.material.emissionStrength = 2.0f;
        surface.model = Mat4::identity();
        packet.items.push_back(surface);
        setCamera(packet);
        packet.lights.clear();
    }
};

struct DiffuseEnvironmentScene {
    std::array<MeshData, 2> meshes;
    RenderPacket packet;

    DiffuseEnvironmentScene() {
        makeQuad(meshes[0], false);
        makeQuad(meshes[1], true);

        DrawItem floor;
        floor.mesh = &meshes[0];
        floor.material.baseColor = {0.78f, 0.72f, 0.64f, 1.0f};
        floor.material.metallic = 0.0f;
        floor.material.roughness = 1.0f;
        floor.model = Mat4::identity();
        packet.items.push_back(floor);

        DrawItem wall;
        wall.mesh = &meshes[1];
        wall.material.baseColor = {0.22f, 0.72f, 0.30f, 1.0f};
        wall.material.metallic = 0.0f;
        wall.material.roughness = 1.0f;
        wall.model = Mat4::identity();
        packet.items.push_back(wall);

        packet.cameraPosition = {0.0f, 0.0f, 3.0f};
        packet.view = lookAt(packet.cameraPosition, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
        packet.projection = perspective(radians(35.0f),
                                        static_cast<float>(kWidth) / kHeight,
                                        0.1f, 100.0f);
        packet.width = kWidth;
        packet.height = kHeight;
        packet.background.color = {0.20f, 0.32f, 0.48f, 1.0f};
        packet.background.strength = 0.7f;
        packet.background.lighting = true;
    }
};

struct AreaEmitterScene {
    std::array<MeshData, 2> meshes;
    RenderPacket packet;

    AreaEmitterScene() {
        makeQuad(meshes[0], false);
        meshes[1].positions = {
            2.0f, -4.0f, 0.05f,
            2.0f,  4.0f, 4.0f,
            2.0f,  4.0f, 0.05f,
            2.0f, -4.0f, 4.0f,
        };
        meshes[1].normals = {
            -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f,
            -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f,
        };
        meshes[1].indices = {0, 1, 2, 0, 3, 1};

        DrawItem floor;
        floor.mesh = &meshes[0];
        floor.material.baseColor = {0.75f, 0.7f, 0.6f, 1.0f};
        floor.model = Mat4::identity();
        packet.items.push_back(floor);

        DrawItem emitter;
        emitter.mesh = &meshes[1];
        emitter.material.baseColor = {0.0f, 0.0f, 0.0f, 1.0f};
        emitter.material.emissionColor = {0.8f, 0.6f, 0.4f, 1.0f};
        emitter.material.emissionStrength = 4.0f;
        emitter.model = Mat4::identity();
        packet.items.push_back(emitter);
        setCamera(packet);
        packet.lights.clear();
    }
};

struct TempTextureAssets {
    std::filesystem::path directory;

    TempTextureAssets() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
                    ("ember-path-tracer-" + std::to_string(stamp));
        std::filesystem::create_directories(directory);
    }
    ~TempTextureAssets() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    void writePpm(const std::string& name, int width, int height,
                  const std::vector<int>& rgb) const {
        std::ofstream file(directory / name, std::ios::binary);
        file << "P3\n" << width << ' ' << height << "\n255\n";
        for (std::size_t i = 0; i < rgb.size(); ++i) {
            file << rgb[i] << ((i + 1) % 3 == 0 ? '\n' : ' ');
        }
    }
};

void setTextureQuadUvs(MeshData& mesh) {
    mesh.uvs = {0.0f, 0.0f, 1.0f, 0.0f,
                1.0f, 1.0f, 0.0f, 1.0f};
}

void makeSlabScene(std::array<MeshData, 2>& meshes, RenderPacket& packet) {
    meshes[0].positions = {-10, -10, 0, 10, -10, 0, 10, 10, 0, -10, 10, 0};
    meshes[0].normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    meshes[0].indices = {0, 1, 2, 0, 2, 3};
    meshes[1].positions = {-10, -10, 2, -10, 10, 2, 10, 10, 2, 10, -10, 2};
    meshes[1].normals = {0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1};
    meshes[1].indices = {0, 1, 2, 0, 2, 3};
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        DrawItem surface;
        surface.mesh = &meshes[i];
        surface.material.baseColor = {0.72f, 0.68f, 0.61f, 1.0f};
        surface.material.roughness = 1.0f;
        surface.model = Mat4::identity();
        packet.items.push_back(surface);
    }
    setCamera(packet);
    packet.cameraPosition = {0, 0, 1};
    packet.view = lookAt(packet.cameraPosition, {0, 0, 0}, {0, 1, 0});
    packet.background.color = {0.35f, 0.42f, 0.5f, 1.0f};
    packet.background.strength = 0.6f;
    packet.background.lighting = true;
}

bool sameImage(const PathTraceImage& a, const PathTraceImage& b) {
    return a.width == b.width && a.height == b.height &&
           a.samplesPerPixel == b.samplesPerPixel && a.linearPixels == b.linearPixels &&
           a.pixelSampleCounts == b.pixelSampleCounts;
}

struct AdaptiveEdgeScene {
    MeshData mesh;
    RenderPacket packet;

    AdaptiveEdgeScene() {
        // A constant-emission half-plane ends through the center camera pixel.
        // Most pixels see a constant value and converge at minSamples; the
        // center pixel receives a deterministic mixture of black and emission.
        mesh.positions = {-100.0f, -100.0f, 0.0f,
                            0.0f, -100.0f, 0.0f,
                            0.0f,  100.0f, 0.0f,
                         -100.0f,  100.0f, 0.0f};
        mesh.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
        mesh.indices = {0, 1, 2, 0, 2, 3};
        DrawItem surface;
        surface.mesh = &mesh;
        surface.model = Mat4::identity();
        surface.material.baseColor = {0, 0, 0, 1};
        surface.material.emissionColor = {1, 0.5f, 0.25f, 1};
        surface.material.emissionStrength = 2.0f;
        packet.items.push_back(surface);
        setCamera(packet);
        packet.background.color = {0, 0, 0, 1};
        packet.background.strength = 0;
        packet.lights.clear();
    }
};

double rmsRgbError(const PathTraceImage& a, const PathTraceImage& b) {
    if (a.width != b.width || a.height != b.height ||
        a.linearPixels.size() != b.linearPixels.size() || a.linearPixels.empty())
        return INFINITY;
    double sumSquared = 0.0;
    for (std::size_t i = 0; i < a.linearPixels.size(); ++i) {
        const Color& x = a.linearPixels[i];
        const Color& y = b.linearPixels[i];
        for (int channel = 0; channel < 3; ++channel) {
            const double d = static_cast<double>(x[channel]) - y[channel];
            sumSquared += d * d;
        }
    }
    return std::sqrt(sumSquared / (3.0 * a.linearPixels.size()));
}

double meanRgb(const PathTraceImage& image) {
    if (image.linearPixels.empty()) return 0.0;
    double sum = 0.0;
    for (const Color& pixel : image.linearPixels)
        sum += static_cast<double>(pixel.r) + pixel.g + pixel.b;
    return sum / (3.0 * image.linearPixels.size());
}

bool finiteAndBounded(const PathTraceImage& image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.linearPixels.size() != static_cast<std::size_t>(image.width * image.height))
        return false;
    for (const Color& pixel : image.linearPixels) {
        for (int channel = 0; channel < 3; ++channel) {
            if (!std::isfinite(pixel[channel]) || pixel[channel] < -1.0e-4f || pixel[channel] > 2.0f)
                return false;
        }
        if (!std::isfinite(pixel.a)) return false;
    }
    return true;
}

PathTraceSettings testSettings() {
    PathTraceSettings settings;
    settings.maxBounces = 4;
    settings.seed = 0x51A7u;
    return settings;
}

} // namespace

TEST(path_tracer_owns_packet_snapshot_after_source_mutation) {
    DiffuseEnvironmentScene controlScene;
    DiffuseEnvironmentScene changedScene;
    controlScene.packet.items[0].material.emissionColor = {0.3f, 0.2f, 0.1f, 1.0f};
    controlScene.packet.items[0].material.emissionStrength = 2.0f;
    changedScene.packet.items[0].material.emissionColor = {0.3f, 0.2f, 0.1f, 1.0f};
    changedScene.packet.items[0].material.emissionStrength = 2.0f;
    PathTracer control;
    PathTracer snapshot;
    const PathTraceSettings settings = testSettings();

    CHECK(control.setScene(controlScene.packet, 8, 8, settings));
    CHECK(snapshot.setScene(changedScene.packet, 8, 8, settings));

    // All these fields are packet-owned or reached through its non-owning mesh
    // pointers. A completed setScene must have copied what future rays need.
    changedScene.meshes[0].positions.assign(12, 0.0f);
    changedScene.meshes[0].indices.clear();
    changedScene.meshes[0].normals.clear();
    changedScene.packet.items[0].material.baseColor = {0.0f, 0.0f, 0.0f, 1.0f};
    changedScene.packet.items[0].material.emissionColor = {7.0f, 3.0f, 1.0f, 1.0f};
    changedScene.packet.items[0].material.emissionStrength = 9.0f;
    changedScene.packet.items[0].model = translation({100.0f, 0.0f, 0.0f});
    changedScene.packet.background.color = {1.0f, 0.0f, 1.0f, 1.0f};
    changedScene.packet.lights.push_back(LightInstance{});
    changedScene.packet.cameraPosition = {100.0f, 100.0f, 100.0f};
    changedScene.packet.view = Mat4::identity();
    changedScene.packet.projection = Mat4::identity();

    CHECK(control.renderSamples(12));
    CHECK(snapshot.renderSamples(12));
    CHECK_EQ(control.image().samplesPerPixel, 12u);
    CHECK_EQ(snapshot.image().samplesPerPixel, 12u);
    CHECK(sameImage(control.image(), snapshot.image()));
}

TEST(path_tracer_keeps_snapshot_after_packet_and_mesh_destruction) {
    DiffuseEnvironmentScene controlScene;
    PathTracer control;
    PathTracer outlivesPacket;
    const PathTraceSettings settings = testSettings();
    CHECK(control.setScene(controlScene.packet, 8, 8, settings));

    {
        DiffuseEnvironmentScene temporaryScene;
        CHECK(outlivesPacket.setScene(temporaryScene.packet, 8, 8, settings));
    } // packet, DrawItems, and their source MeshData are all destroyed here.

    CHECK(control.renderSamples(10));
    CHECK(outlivesPacket.renderSamples(10));
    CHECK(sameImage(control.image(), outlivesPacket.image()));
}

TEST(path_tracer_is_seed_deterministic_and_accumulates_progressively) {
    DiffuseEnvironmentScene sceneA;
    DiffuseEnvironmentScene sceneB;
    PathTracer progressive;
    PathTracer sameSeed;
    const PathTraceSettings settings = testSettings();

    CHECK(progressive.setScene(sceneA.packet, 12, 8, settings));
    CHECK(sameSeed.setScene(sceneB.packet, 12, 8, settings));
    CHECK_EQ(progressive.image().samplesPerPixel, 0u);
    CHECK_EQ(progressive.image().pixelSampleCounts.size(), 12u * 8u);
    CHECK(progressive.renderSamples(1));
    CHECK_EQ(progressive.image().samplesPerPixel, 1u);
    CHECK(std::all_of(progressive.image().pixelSampleCounts.begin(),
                      progressive.image().pixelSampleCounts.end(),
                      [](u32 count) { return count == 1; }));
    CHECK(progressive.renderSamples(3));
    CHECK_EQ(progressive.image().samplesPerPixel, 4u);
    CHECK(progressive.renderSamples(5));
    CHECK_EQ(progressive.image().samplesPerPixel, 9u);
    CHECK(sameSeed.renderSamples(9));
    CHECK_EQ(sameSeed.image().samplesPerPixel, 9u);
    CHECK(sameImage(progressive.image(), sameSeed.image()));

    progressive.reset();
    CHECK_EQ(progressive.image().samplesPerPixel, 0u);
    CHECK(progressive.renderSamples(9));
    CHECK(sameImage(progressive.image(), sameSeed.image()));
}

TEST(path_tracer_adaptive_sampling_skips_converged_pixels_deterministically) {
    AdaptiveEdgeScene scene;
    PathTraceSettings pathSettings = testSettings();
    pathSettings.maxBounces = 0;
    PathTracer adaptive;
    PathTracer uniformReference;
    CHECK(adaptive.setScene(scene.packet, 23, 15, pathSettings));
    CHECK(uniformReference.setScene(scene.packet, 23, 15, pathSettings));

    PathAdaptiveSamplingSettings adaptiveSettings;
    adaptiveSettings.minSamples = 2;
    adaptiveSettings.maxSamples = 12;
    adaptiveSettings.absoluteError = 0.0;
    adaptiveSettings.relativeError = 0.0;

    std::vector<PathTraceImage> uniformPrefixes;
    uniformPrefixes.reserve(static_cast<std::size_t>(adaptiveSettings.maxSamples));
    for (ember::u32 sample = 1; sample <= adaptiveSettings.maxSamples; ++sample) {
        CHECK(uniformReference.renderSamples(1));
        uniformPrefixes.push_back(uniformReference.image());
    }

    CHECK(!adaptive.adaptiveComplete(adaptiveSettings));
    ember::u32 previousMinimum = 0;
    while (!adaptive.adaptiveComplete(adaptiveSettings)) {
        const std::vector<ember::u32> previousCounts = adaptive.image().pixelSampleCounts;
        CHECK(adaptive.renderAdaptiveSample(adaptiveSettings));
        const PathTraceImage& image = adaptive.image();
        CHECK_EQ(image.pixelSampleCounts.size(), 23u * 15u);
        for (std::size_t pixel = 0; pixel < image.pixelSampleCounts.size(); ++pixel) {
            const ember::u32 count = image.pixelSampleCounts[pixel];
            CHECK(count == previousCounts[pixel] || count == previousCounts[pixel] + 1);
            CHECK(count <= adaptiveSettings.maxSamples);
            if (count > 0) {
                const PathTraceImage& prefix = uniformPrefixes[count - 1];
                CHECK(image.linearPixels[pixel] == prefix.linearPixels[pixel]);
            }
        }
        CHECK(image.samplesPerPixel >= previousMinimum);
        previousMinimum = image.samplesPerPixel;
    }

    const PathTraceImage result = adaptive.image();
    const ember::u32 minimum = *std::min_element(result.pixelSampleCounts.begin(),
                                                  result.pixelSampleCounts.end());
    const ember::u32 maximum = *std::max_element(result.pixelSampleCounts.begin(),
                                                  result.pixelSampleCounts.end());
    CHECK_EQ(result.samplesPerPixel, minimum);
    CHECK_EQ(minimum, adaptiveSettings.minSamples);
    CHECK_EQ(maximum, adaptiveSettings.maxSamples);
    CHECK(adaptive.adaptiveComplete(adaptiveSettings));
    CHECK(adaptive.renderAdaptiveSample(adaptiveSettings)); // completed calls add no samples
    CHECK(sameImage(adaptive.image(), result));

    PathAdaptiveSamplingSettings invalid = adaptiveSettings;
    invalid.minSamples = 0;
    CHECK(!adaptive.renderAdaptiveSample(invalid));
    invalid = adaptiveSettings;
    invalid.maxSamples = adaptiveSettings.minSamples - 1;
    CHECK(!adaptive.renderAdaptiveSample(invalid));
    invalid = adaptiveSettings;
    invalid.maxSamples = (1u << 24) + 1ull;
    CHECK(!adaptive.renderAdaptiveSample(invalid));
}

TEST(path_tracer_does_not_clamp_the_progressive_radiance_sum) {
    DiffuseEnvironmentScene scene;
    scene.packet.items.clear();
    scene.packet.background.color = {1000000.0f, 1000000.0f, 1000000.0f, 1.0f};
    scene.packet.background.strength = 1.0f;

    PathTracer tracer;
    CHECK(tracer.setScene(scene.packet, 2, 2, testSettings()));
    CHECK(tracer.renderSamples(1));
    const float firstSample = tracer.image().linearPixels.front().r;
    CHECK(tracer.renderSamples(1));
    const float twoSampleMean = tracer.image().linearPixels.front().r;

    CHECK_NEAR(firstSample, 1000000.0f, 2.0f);
    CHECK_NEAR(twoSampleMean, firstSample, 2.0f);
}

TEST(path_tracer_diffuse_environment_energy_is_finite_bounded_and_converges) {
    DiffuseEnvironmentScene scene;
    PathTracer tracer;
    CHECK(tracer.setScene(scene.packet, kWidth, kHeight, testSettings()));

    CHECK(tracer.renderSamples(8));
    CHECK_EQ(tracer.image().samplesPerPixel, 8u);
    const PathTraceImage eightSamples = tracer.image();
    CHECK(finiteAndBounded(eightSamples));

    CHECK(tracer.renderSamples(56));
    CHECK_EQ(tracer.image().samplesPerPixel, 64u);
    const PathTraceImage sixtyFourSamples = tracer.image();
    CHECK(finiteAndBounded(sixtyFourSamples));

    CHECK(tracer.renderSamples(448));
    CHECK_EQ(tracer.image().samplesPerPixel, 512u);
    const PathTraceImage fiveHundredTwelveSamples = tracer.image();
    CHECK(finiteAndBounded(fiveHundredTwelveSamples));

    // Use the same deterministic sequence at higher spp as a numerical reference.
    // With the bounded environment/materials above, these deliberately generous
    // linear-RGB RMS limits allow ordinary finite-sample noise while requiring
    // each larger budget to settle inside a tighter neighbourhood of the mean.
    CHECK(tracer.renderSamples(1536));
    CHECK_EQ(tracer.image().samplesPerPixel, 2048u);
    const PathTraceImage reference = tracer.image();
    CHECK(finiteAndBounded(reference));
    CHECK(meanRgb(reference) > 0.01); // the diffuse surface receives nonzero environment energy
    CHECK(rmsRgbError(eightSamples, reference) <= 0.20);
    CHECK(rmsRgbError(sixtyFourSamples, reference) <= 0.10);
    CHECK(rmsRgbError(fiveHundredTwelveSamples, reference) <= 0.05);
}

TEST(path_tracer_bvh_known_geometry_emission_uses_linear_color_times_strength) {
    DiffuseEnvironmentScene scene;
    scene.packet.items.resize(1);
    DrawItem& surface = scene.packet.items[0];
    surface.material.baseColor = {0.0f, 0.0f, 0.0f, 1.0f};
    surface.material.emissionColor = {2.0f, 1.0f, 0.5f, 1.0f};
    surface.material.emissionStrength = 3.0f;
    scene.packet.lights.clear();
    scene.packet.background.color = {0.0f, 0.0f, 0.0f, 1.0f};
    scene.packet.background.strength = 0.0f;

    PathTraceSettings settings = testSettings();
    settings.maxBounces = 0;
    PathTracer tracer;
    CHECK(tracer.setScene(scene.packet, 8, 8, settings));
    CHECK(tracer.renderSamples(1));
    for (const Color& pixel : tracer.image().linearPixels) {
        CHECK_NEAR(pixel.r, 6.0f, 1.0e-5f);
        CHECK_NEAR(pixel.g, 3.0f, 1.0e-5f);
        CHECK_NEAR(pixel.b, 1.5f, 1.0e-5f);
    }
}

TEST(path_tracer_next_event_estimation_samples_off_camera_emission) {
    EmissionBounceScene scene;
    PathTraceSettings settings = testSettings();
    settings.maxBounces = 0;
    PathTracer primaryOnly;
    CHECK(primaryOnly.setScene(scene.packet, 8, 8, settings));
    CHECK(primaryOnly.renderSamples(64));
    // The emitter is outside the camera view, but explicit area-light NEE now
    // reaches it from the floor even before a BSDF continuation is traced.
    CHECK(meanRgb(primaryOnly.image()) > 0.0);

    settings.maxBounces = 1;
    PathTracer oneBounce;
    CHECK(oneBounce.setScene(scene.packet, 8, 8, settings));
    CHECK(oneBounce.renderSamples(64));
    // The emitter is not in the camera view. The higher-bounce run additionally
    // exercises BSDF paths to that emitter with the complementary MIS weight.
    CHECK(meanRgb(oneBounce.image()) > 0.0);
    CHECK(meanRgb(oneBounce.image()) <= 3.5);
    for (const Color& pixel : oneBounce.image().linearPixels) {
        CHECK(std::isfinite(pixel.r) && std::isfinite(pixel.g) && std::isfinite(pixel.b));
        CHECK(pixel.r >= 0.0f && pixel.g >= 0.0f && pixel.b >= 0.0f);
    }
}

TEST(path_tracer_invalid_emission_components_are_ignored) {
    DiffuseEnvironmentScene scene;
    scene.packet.items.resize(1);
    DrawItem& surface = scene.packet.items[0];
    surface.material.baseColor = {0.0f, 0.0f, 0.0f, 1.0f};
    surface.material.emissionColor = {-4.0f, NAN, 2.0f, 1.0f};
    surface.material.emissionStrength = 3.0f;
    scene.packet.lights.clear();
    scene.packet.background.color = {0.0f, 0.0f, 0.0f, 1.0f};
    scene.packet.background.strength = 0.0f;
    PathTraceSettings settings = testSettings();
    settings.maxBounces = 0;
    PathTracer tracer;
    CHECK(tracer.setScene(scene.packet, 2, 2, settings));
    CHECK(tracer.renderSamples(1));
    for (const Color& pixel : tracer.image().linearPixels) {
        CHECK_NEAR(pixel.r, 0.0f, 1.0e-7f);
        CHECK_NEAR(pixel.g, 0.0f, 1.0e-7f);
        CHECK_NEAR(pixel.b, 6.0f, 1.0e-5f);
    }

    surface.material.emissionColor = {1.0f, 1.0f, 1.0f, 1.0f};
    surface.material.emissionStrength = INFINITY;
    PathTracer invalidStrength;
    CHECK(invalidStrength.setScene(scene.packet, 2, 2, settings));
    CHECK(invalidStrength.renderSamples(1));
    CHECK_NEAR(meanRgb(invalidStrength.image()), 0.0, 1.0e-7);

    surface.material.emissionStrength = -2.0f;
    PathTracer negativeStrength;
    CHECK(negativeStrength.setScene(scene.packet, 2, 2, settings));
    CHECK(negativeStrength.renderSamples(1));
    CHECK_NEAR(meanRgb(negativeStrength.image()), 0.0, 1.0e-7);
}

TEST(path_tracer_bvh_handles_thousands_of_triangles_without_timing_assumptions) {
    LargeTiledEmissionScene scene(64); // 8,192 triangles in the owned BVH.
    PathTraceSettings settings = testSettings();
    settings.maxBounces = 0;
    PathTracer tracer;
    CHECK(tracer.setScene(scene.packet, 12, 8, settings));
    CHECK(tracer.renderSamples(3));
    CHECK_EQ(tracer.image().samplesPerPixel, 3u);
    CHECK(finiteAndBounded(tracer.image()));
    for (const Color& pixel : tracer.image().linearPixels) {
        CHECK_NEAR(pixel.r, 0.5f, 1.0e-5f);
        CHECK_NEAR(pixel.g, 1.0f, 1.0e-5f);
        CHECK_NEAR(pixel.b, 1.5f, 1.0e-5f);
    }
}

TEST(path_tracer_dielectric_transmission_refracts_into_emitter) {
    AreaEmitterScene scene;
    scene.packet.items.resize(2);
    DrawItem& glass = scene.packet.items[0];
    glass.material.baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
    glass.material.metallic = 0.0f;
    glass.material.roughness = 0.5f;
    glass.material.transmission = 1.0f;
    glass.material.ior = 1.45f;

    // Put a broad emitter behind the transmissive plane. A transmitted camera
    // path reaches it; a reflected path returns to the black environment.
    MeshData& back = scene.meshes[1];
    back.positions = {
        -20.0f, -20.0f, -2.0f, 20.0f, -20.0f, -2.0f,
         20.0f,  20.0f, -2.0f, -20.0f, 20.0f, -2.0f,
    };
    back.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    back.indices = {0, 1, 2, 0, 2, 3};
    DrawItem& emitter = scene.packet.items[1];
    emitter.mesh = &back;
    emitter.material.baseColor = {0, 0, 0, 1};
    emitter.material.emissionColor = {1.0f, 0.8f, 0.6f, 1.0f};
    emitter.material.emissionStrength = 2.0f;
    scene.packet.background.color = {0, 0, 0, 1};
    scene.packet.background.strength = 0.0f;

    PathTraceSettings settings = testSettings();
    settings.maxBounces = 0;
    PathTracer nextEventOnly;
    CHECK(nextEventOnly.setScene(scene.packet, 1, 1, settings));
    CHECK(nextEventOnly.renderSamples(256));
    settings.maxBounces = 1;
    PathTracer tracer;
    CHECK(tracer.setScene(scene.packet, 1, 1, settings));
    CHECK(tracer.renderSamples(256));
    const float neeOnly = nextEventOnly.image().linearPixels.front().r;
    const float throughGlass = tracer.image().linearPixels.front().r;
    CHECK(std::isfinite(neeOnly) && std::isfinite(throughGlass));
    CHECK(throughGlass > neeOnly + 0.1f);
}

TEST(path_tracer_rough_glossy_direct_response_changes_with_roughness) {
    DiffuseEnvironmentScene scene;
    scene.packet.items.resize(1);
    DrawItem& surface = scene.packet.items[0];
    surface.material.baseColor = {0.9f, 0.8f, 0.7f, 1.0f};
    surface.material.metallic = 1.0f;
    surface.material.roughness = 0.16f;
    scene.packet.lights.clear();
    LightInstance sun{};
    sun.type = LightType::Directional;
    sun.direction = {0.0f, 0.0f, -1.0f}; // rays travel down; source is above the plane
    sun.color = {1.0f, 1.0f, 1.0f, 1.0f};
    sun.intensity = 1.0f;
    scene.packet.lights.push_back(sun);

    PathTraceSettings settings = testSettings();
    settings.maxBounces = 0;
    PathTracer polished;
    CHECK(polished.setScene(scene.packet, 1, 1, settings));
    CHECK(polished.renderSamples(8));
    scene.packet.items[0].material.roughness = 0.8f;
    PathTracer rough;
    CHECK(rough.setScene(scene.packet, 1, 1, settings));
    CHECK(rough.renderSamples(8));
    CHECK(std::isfinite(polished.image().linearPixels.front().r));
    CHECK(std::isfinite(rough.image().linearPixels.front().r));
    CHECK(meanRgb(polished.image()) > meanRgb(rough.image()) * 2.0);
}

TEST(path_tracer_area_light_nee_agrees_with_bsdf_hit_estimator) {
    AreaEmitterScene scene;
    PathTraceSettings directSettings = testSettings();
    directSettings.maxBounces = 0; // NEE only
    PathTraceSettings combinedSettings = directSettings;
    combinedSettings.maxBounces = 1; // NEE plus the MIS-weighted BSDF-hit strategy
    PathTracer directOnly;
    PathTracer combined;
    CHECK(directOnly.setScene(scene.packet, 1, 1, directSettings));
    CHECK(combined.setScene(scene.packet, 1, 1, combinedSettings));
    CHECK(directOnly.renderSamples(8192));
    CHECK(combined.renderSamples(8192));
    const double direct = meanRgb(directOnly.image());
    const double both = meanRgb(combined.image());
    CHECK(direct > 0.01);
    CHECK(both > 0.01);
    CHECK(std::fabs(direct - both) <= std::max(0.05, direct * 0.3));
}

TEST(path_tracer_emissive_hit_uses_complementary_mis_weight) {
    AreaEmitterScene scene;
    PathTraceSettings neeSettings = testSettings();
    neeSettings.seed = 0xE771u;
    neeSettings.maxBounces = 0;
    PathTraceSettings hitSettings = neeSettings;
    hitSettings.maxBounces = 1;
    PathTracer nee;
    PathTracer neeAndBsdfHits;
    CHECK(nee.setScene(scene.packet, 1, 1, neeSettings));
    CHECK(neeAndBsdfHits.setScene(scene.packet, 1, 1, hitSettings));
    CHECK(nee.renderSamples(4096));
    CHECK(neeAndBsdfHits.renderSamples(4096));
    const double neeEstimate = meanRgb(nee.image());
    const double combinedEstimate = meanRgb(neeAndBsdfHits.image());
    CHECK(std::isfinite(neeEstimate));
    CHECK(std::isfinite(combinedEstimate));
    CHECK(neeEstimate > 0.01);
    // Adding the BSDF technique must not count the same area-light paths again
    // at full strength; the complementary power-heuristic weight keeps the
    // deterministic finite-sample estimate in the same energy range.
    CHECK(combinedEstimate > neeEstimate * 0.5 - 0.05);
    CHECK(combinedEstimate < neeEstimate * 1.5 + 0.05);
}

TEST(path_tracer_constant_environment_nee_matches_bsdf_misses) {
    DiffuseEnvironmentScene scene;
    scene.packet.items.resize(1); // no geometry can occlude the constant environment
    scene.packet.background.color = {0.2f, 0.32f, 0.48f, 1.0f};
    scene.packet.background.strength = 0.7f;
    scene.packet.background.lighting = true;
    PathTraceSettings neeSettings = testSettings();
    neeSettings.maxBounces = 0;
    PathTraceSettings combinedSettings = neeSettings;
    combinedSettings.maxBounces = 1;
    PathTracer nee;
    PathTracer neeAndBsdfMisses;
    CHECK(nee.setScene(scene.packet, 1, 1, neeSettings));
    CHECK(neeAndBsdfMisses.setScene(scene.packet, 1, 1, combinedSettings));
    CHECK(nee.renderSamples(8192));
    CHECK(neeAndBsdfMisses.renderSamples(8192));
    const double neeEstimate = meanRgb(nee.image());
    const double combinedEstimate = meanRgb(neeAndBsdfMisses.image());
    CHECK(neeEstimate > 0.01);
    CHECK_NEAR(neeEstimate, combinedEstimate, std::max(0.03, neeEstimate * 0.15));
}

TEST(path_tracer_bsdf_area_light_batches_are_reproducible) {
    AreaEmitterScene sceneA;
    AreaEmitterScene sceneB;
    PathTraceSettings settings = testSettings();
    settings.maxBounces = 2;
    PathTracer progressive;
    PathTracer oneBatch;
    CHECK(progressive.setScene(sceneA.packet, 3, 2, settings));
    CHECK(oneBatch.setScene(sceneB.packet, 3, 2, settings));
    CHECK(progressive.renderSamples(7));
    CHECK(progressive.renderSamples(9));
    CHECK(oneBatch.renderSamples(16));
    CHECK(sameImage(progressive.image(), oneBatch.image()));
}

TEST(path_tracer_interpolates_uvs_and_owns_decoded_base_color_images) {
    TempTextureAssets assets;
    assets.writePpm("base.ppm", 2, 1, {255, 0, 0, 0, 0, 255});
    DiffuseEnvironmentScene scene;
    scene.packet.items.resize(1);
    setTextureQuadUvs(scene.meshes[0]);
    scene.packet.items[0].material.baseColor = {1, 1, 1, 1};
    scene.packet.items[0].material.baseColorTexture =
        ImageTextureRef{"base.ppm", ImageColorSpace::SRgb};
    scene.packet.assetBaseDirectory = assets.directory.string();
    scene.packet.lights.clear();
    LightInstance sun{};
    sun.type = LightType::Directional;
    sun.direction = {0, 0, -1};
    sun.color = {1, 1, 1, 1};
    sun.intensity = 1.0f;
    scene.packet.lights.push_back(sun);

    PathTraceSettings settings = testSettings();
    settings.maxBounces = 0;
    PathTracer beforeMutation;
    PathTracer snapshot;
    CHECK(beforeMutation.setScene(scene.packet, kWidth, kHeight, settings));
    CHECK(snapshot.setScene(scene.packet, kWidth, kHeight, settings));
    CHECK(beforeMutation.renderSamples(8));

    // A setScene snapshot owns decoded pixels, not a file/path dependency.
    assets.writePpm("base.ppm", 2, 1, {0, 255, 0, 0, 255, 0});
    scene.packet.items[0].material.baseColorTexture.path = "missing-after-snapshot.ppm";
    CHECK(snapshot.renderSamples(8));
    CHECK(sameImage(beforeMutation.image(), snapshot.image()));

    const Color left = beforeMutation.image().linearPixels[8 * kWidth + 1];
    const Color right = beforeMutation.image().linearPixels[8 * kWidth + 22];
    CHECK(left.r > left.b * 2.5f);
    CHECK(right.b > right.r * 2.5f);
}

TEST(path_tracer_latlong_environment_importance_sampling_has_matching_mis) {
    TempTextureAssets assets;
    std::vector<int> pixels;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 8; ++x) {
            pixels.insert(pixels.end(), {40 + x * 24, 35 + y * 32, 90 + x * 12});
        }
    }
    assets.writePpm("world.ppm", 8, 4, pixels);
    DiffuseEnvironmentScene scene;
    scene.packet.items.resize(1); // no geometry occludes a sampled environment direction
    scene.packet.background.color = {0, 0, 0, 1};
    scene.packet.background.strength = 0.7f;
    scene.packet.background.lighting = true;
    scene.packet.background.environmentTexture =
        ImageTextureRef{"world.ppm", ImageColorSpace::SRgb};
    scene.packet.assetBaseDirectory = assets.directory.string();

    PathTraceSettings neeSettings = testSettings();
    neeSettings.maxBounces = 0;
    PathTraceSettings combinedSettings = neeSettings;
    combinedSettings.maxBounces = 1;
    PathTracer nee;
    PathTracer neeAndBsdfMisses;
    CHECK(nee.setScene(scene.packet, 1, 1, neeSettings));
    CHECK(neeAndBsdfMisses.setScene(scene.packet, 1, 1, combinedSettings));
    CHECK(nee.renderSamples(8192));
    CHECK(neeAndBsdfMisses.renderSamples(8192));
    const double directEstimate = meanRgb(nee.image());
    const double combinedEstimate = meanRgb(neeAndBsdfMisses.image());
    CHECK(directEstimate > 0.01);
    CHECK(combinedEstimate > 0.01);
    CHECK(std::fabs(directEstimate - combinedEstimate) <=
          std::max(0.05, directEstimate * 0.2));

    // The new environment sampler and roulette preserve sample-index seeds
    // regardless of how a progressive batch is partitioned.
    PathTraceSettings batchSettings = testSettings();
    batchSettings.maxBounces = 3;
    batchSettings.rouletteStartBounce = 0;
    PathTracer progressive;
    PathTracer oneBatch;
    CHECK(progressive.setScene(scene.packet, 2, 2, batchSettings));
    CHECK(oneBatch.setScene(scene.packet, 2, 2, batchSettings));
    CHECK(progressive.renderSamples(3));
    CHECK(progressive.renderSamples(5));
    CHECK(oneBatch.renderSamples(8));
    CHECK(sameImage(progressive.image(), oneBatch.image()));
}

TEST(path_tracer_russian_roulette_is_finite_and_compensated_in_expectation) {
    std::array<MeshData, 2> meshes;
    RenderPacket packet;
    makeSlabScene(meshes, packet);
    PathTraceSettings rouletteSettings = testSettings();
    rouletteSettings.maxBounces = 12;
    rouletteSettings.rouletteStartBounce = 3;
    PathTraceSettings referenceSettings = rouletteSettings;
    referenceSettings.rouletteStartBounce = 64; // disable before the terminal bounce

    PathTracer roulette;
    PathTracer fixedDepth;
    CHECK(roulette.setScene(packet, 1, 1, rouletteSettings));
    CHECK(fixedDepth.setScene(packet, 1, 1, referenceSettings));
    CHECK(roulette.renderSamples(16384));
    CHECK(fixedDepth.renderSamples(16384));
    CHECK(finiteAndBounded(roulette.image()));
    CHECK(finiteAndBounded(fixedDepth.image()));
    const double expected = meanRgb(fixedDepth.image());
    const double sampled = meanRgb(roulette.image());
    CHECK(expected > 0.01);
    CHECK(std::fabs(sampled - expected) <= std::max(0.04, expected * 0.15));
}

HARNESS_MAIN()
