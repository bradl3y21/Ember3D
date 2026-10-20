#pragma once

#include <cstddef>
#include <limits>
#include <vector>

#include "core/Types.h"
#include "render/PathSampling.h"
#include "render/TextureImage.h"

namespace ember::render {

struct PathTraceSettings {
  ember::u32 maxBounces = 4;
  ember::u32 seed = 1;
  // Zero-based surface-bounce index at which continuation roulette begins.
  // The default of 3 leaves the first three continuations untouched; 64
  // disables roulette for traces whose maximum bounce count is <= 64.
  ember::u32 rouletteStartBounce = 3;
};

struct PathTraceImage {
  int width = 0;
  int height = 0;
  // Uniform path: the common spp count. Adaptive path: minimum pixel spp.
  ember::u32 samplesPerPixel = 0;
  std::vector<ember::Color> linearPixels;
  // Per-pixel number of radiance observations represented by linearPixels.
  // Uniform renders keep every entry equal; adaptive renders may differ.
  std::vector<ember::u32> pixelSampleCounts;
};

// A synchronous, CPU-only progressive path tracer. Scene packet pointers are
// read only during setScene(); the tracer keeps its own geometry/material/light
// snapshot and deterministic BVH afterwards. It supports diffuse, rough glossy
// and dielectric transport and next-event estimation for resolved lights,
// emissive triangles and an enabled environment. Delta point/directional
// lights are summed without MIS; emitters use a uniform combined-area proposal
// and the constant environment a uniform-sphere proposal. Lat-long environment
// images use a luminance-times-solid-angle texel proposal. Both use the power
// heuristic against the BSDF solid-angle PDF, with complementary weights on
// BSDF emitter/environment hits. Russian roulette starts after three completed
// scattering events by default and compensates surviving throughput by the
// reciprocal survival probability. Unsupported: node graphs, normal maps,
// integrated denoising, and production-grade Cycles behavior.
// This is not a production path tracer or Cycles/Eevee parity.
class PathTracer {
public:
  bool setScene(const ember::RenderPacket&, int width, int height,
                const PathTraceSettings& settings = {});
  void reset();
  bool renderSamples(ember::u32 sampleCount = 1);
  // Adds one deterministic sample to each pixel that has not converged or
  // reached maxSamples. Adaptive images report the minimum pixel sample count.
  bool renderAdaptiveSample(const PathAdaptiveSamplingSettings& settings);
  bool adaptiveComplete(const PathAdaptiveSamplingSettings& settings) const;
  const PathTraceImage& image() const;

private:
  struct Triangle {
    ember::Vec3 p0, p1, p2;
    ember::Vec3 n0, n1, n2;
    ember::Vec2 uv0{}, uv1{}, uv2{};
    bool hasUvs = false;
    ember::Vec3 boundsMin, boundsMax, centroid;
    ember::MaterialDesc material;
    std::size_t textureIndex = std::numeric_limits<std::size_t>::max();
  };

  struct BvhNode {
    ember::Vec3 boundsMin, boundsMax;
    std::size_t first = 0;
    std::size_t count = 0;
    std::size_t left = 0;
    std::size_t right = 0;
  };

  struct Hit {
    float distance = std::numeric_limits<float>::infinity();
    float u = 0.0f;
    float v = 0.0f;
    std::size_t triangleIndex = 0;
  };

  struct Emitter {
    std::size_t triangleIndex = 0;
    double cumulativeArea = 0.0;
  };

  struct EnvironmentTexel {
    double cumulativeWeight = 0.0;
    double probability = 0.0;
    double solidAngle = 0.0;
  };

  bool intersect(const ember::Ray& ray, float maxDistance, Hit& hit) const;
  bool renderSamplesImpl(ember::u32 sampleCount,
                         const PathAdaptiveSamplingSettings* adaptiveSettings);

  std::vector<Triangle> triangles_;
  std::vector<Emitter> emitters_;
  std::vector<double> triangleEmitterAreas_;
  double totalEmitterArea_ = 0.0;
  std::vector<std::size_t> bvhTriangleIndices_;
  std::vector<BvhNode> bvhNodes_;
  std::vector<ember::LightInstance> lights_;
  std::vector<TextureImage> materialTextures_;
  TextureImage environmentTexture_;
  std::vector<EnvironmentTexel> environmentTexels_;
  double environmentWeight_ = 0.0;
  std::vector<ember::Vec3> accumulatedRadiance_;
  PathAdaptiveSampler adaptiveSampler_;
  ember::Mat4 inverseViewProjection_;
  ember::Vec3 cameraPosition_{};
  ember::Color backgroundColor_{};
  float backgroundStrength_ = 1.0f;
  bool backgroundLighting_ = false;
  bool perspectiveCamera_ = true;
  PathTraceSettings settings_{};
  PathTraceImage image_{};
  bool sceneReady_ = false;
};

} // namespace ember::render
