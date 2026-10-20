#include "render/PathTracer.h"

#include "render/PathBsdf.h"
#include "render/TextureImage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace ember::render {

namespace {

constexpr int kMaxDimension = 8192;
constexpr std::size_t kMaxPixelCount = 16u * 1024u * 1024u;
constexpr ember::u32 kMaxBounces = 64;
constexpr ember::u32 kMaxSamplesPerBatch = 1u << 20;
constexpr ember::u32 kMaxSamplesPerPixel = 1u << 24;
constexpr float kRayEpsilon = 1.0e-4f;
constexpr float kMaxRadiance = 1.0e6f;
constexpr std::size_t kNoTexture = std::numeric_limits<std::size_t>::max();

bool finite(float v) {
  return std::isfinite(v);
}

bool finite(ember::Vec3 v) {
  return finite(v.x) && finite(v.y) && finite(v.z);
}

bool finite(const ember::Mat4& m) {
  for (float v : m.m) {
    if (!finite(v)) return false;
  }
  return true;
}

float nonnegativeFinite(float v, float fallback = 0.0f) {
  return finite(v) ? ember::clampf(v, 0.0f, kMaxRadiance) : fallback;
}

float emissionComponent(float v) {
  return finite(v) && v >= 0.0f ? v : 0.0f;
}

float boundedEmissionProduct(float color, float strength) {
  const double product = static_cast<double>(color) * strength;
  return static_cast<float>(std::min(product, static_cast<double>(kMaxRadiance)));
}

double triangleArea(const ember::Vec3& p0, const ember::Vec3& p1,
                    const ember::Vec3& p2) {
  const double ax = static_cast<double>(p1.x) - p0.x;
  const double ay = static_cast<double>(p1.y) - p0.y;
  const double az = static_cast<double>(p1.z) - p0.z;
  const double bx = static_cast<double>(p2.x) - p0.x;
  const double by = static_cast<double>(p2.y) - p0.y;
  const double bz = static_cast<double>(p2.z) - p0.z;
  const double cx = ay * bz - az * by;
  const double cy = az * bx - ax * bz;
  const double cz = ax * by - ay * bx;
  const double area = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
  return std::isfinite(area) ? area : 0.0;
}

float powerHeuristic(float a, float b) {
  if (!(a > 0.0f)) return 0.0f;
  if (!(b > 0.0f)) return 1.0f;
  const double aa = static_cast<double>(a) * a;
  const double bb = static_cast<double>(b) * b;
  return static_cast<float>(aa / (aa + bb));
}

ember::Vec2 environmentUv(ember::Vec3 direction) {
  // Lat-long convention matches TextureImage's bottom-origin UVs and top-down
  // storage: +X is u=0.5, +Z is u=0.75, and +Y is v=1 (top row).
  direction = ember::normalize(direction);
  const float phi = std::atan2(direction.z, direction.x);
  return {phi / ember::kTwoPi + 0.5f,
          1.0f - std::acos(ember::clampf(direction.y, -1.0f, 1.0f)) / ember::kPi};
}

float luminance(const ember::Color& color) {
  return ember::maxf(0.0f, 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b);
}

float component(ember::Vec3 v, int axis) {
  return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

ember::Vec3 boundedNonnegative(ember::Vec3 v) {
  return {nonnegativeFinite(v.x), nonnegativeFinite(v.y), nonnegativeFinite(v.z)};
}

ember::Vec3 clampRadiance(ember::Vec3 v) {
  return {nonnegativeFinite(v.x), nonnegativeFinite(v.y), nonnegativeFinite(v.z)};
}

bool intersectBounds(ember::Vec3 boundsMin, ember::Vec3 boundsMax,
                     const ember::Ray& ray, float maxDistance, float& nearDistance) {
  double nearValue = 0.0;
  double farValue = static_cast<double>(maxDistance);
  for (int axis = 0; axis < 3; ++axis) {
    const double origin = component(ray.origin, axis);
    const double direction = component(ray.direction, axis);
    const double minimum = component(boundsMin, axis);
    const double maximum = component(boundsMax, axis);
    if (std::fabs(direction) <= 1.0e-12) {
      if (origin < minimum || origin > maximum) return false;
      continue;
    }
    double first = (minimum - origin) / direction;
    double last = (maximum - origin) / direction;
    if (first > last) std::swap(first, last);
    nearValue = std::max(nearValue, first);
    farValue = std::min(farValue, last);
    if (farValue < nearValue) return false;
  }
  if (farValue < 0.0 || nearValue > static_cast<double>(maxDistance)) return false;
  nearDistance = static_cast<float>(nearValue);
  return true;
}

ember::u32 hash32(ember::u32 x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

struct Random {
  ember::u32 state;

  ember::u32 nextU32() {
    state += 0x9e3779b9u;
    return hash32(state);
  }

  float nextFloat() {
    return (static_cast<float>(nextU32() >> 8) + 0.5f) * (1.0f / 16777216.0f);
  }
};

bool unproject(const ember::Mat4& inverseViewProjection, float x, float y,
               float z, ember::Vec3& point) {
  const ember::Vec4 homogeneous = inverseViewProjection * ember::Vec4{x, y, z, 1.0f};
  if (!finite(homogeneous.x) || !finite(homogeneous.y) || !finite(homogeneous.z) ||
      !finite(homogeneous.w) || std::fabs(homogeneous.w) <= 1.0e-8f) {
    return false;
  }
  point = {homogeneous.x / homogeneous.w,
           homogeneous.y / homogeneous.w,
           homogeneous.z / homogeneous.w};
  return finite(point);
}

ember::MaterialDesc sanitizedMaterial(ember::MaterialDesc material) {
  const ember::Vec3 base = material.baseColor.rgb();
  material.baseColor.r = finite(base.x) ? ember::clampf(base.x, 0.0f, 1.0f) : 0.8f;
  material.baseColor.g = finite(base.y) ? ember::clampf(base.y, 0.0f, 1.0f) : 0.8f;
  material.baseColor.b = finite(base.z) ? ember::clampf(base.z, 0.0f, 1.0f) : 0.8f;
  material.baseColor.a = 1.0f;
  material.metallic = finite(material.metallic) ? ember::clampf(material.metallic, 0.0f, 1.0f) : 0.0f;
  material.roughness = finite(material.roughness) ? ember::clampf(material.roughness, 0.0f, 1.0f) : 0.5f;
  material = ember::sanitizedMaterialDesc(material);
  return material;
}

} // namespace

bool PathTracer::setScene(const ember::RenderPacket& packet, int width, int height,
                          const PathTraceSettings& settings) {
  if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension ||
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) > kMaxPixelCount ||
      settings.maxBounces > kMaxBounces || settings.rouletteStartBounce > kMaxBounces ||
      !finite(packet.cameraPosition) ||
      !finite(packet.view) || !finite(packet.projection)) {
    return false;
  }

  const ember::Mat4 viewProjection = packet.projection * packet.view;
  const ember::Mat4 inverseViewProjection = ember::inverse(viewProjection);
  if (!finite(inverseViewProjection)) return false;

  const ember::Mat4 identityCheck = inverseViewProjection * viewProjection;
  for (int i = 0; i < 16; ++i) {
    const float expected = (i % 5 == 0) ? 1.0f : 0.0f;
    if (!finite(identityCheck.m[i]) || std::fabs(identityCheck.m[i] - expected) > 1.0e-2f) {
      return false;
    }
  }

  ember::Vec3 nearPoint, farPoint;
  if (!unproject(inverseViewProjection, 0.0f, 0.0f, -1.0f, nearPoint) ||
      !unproject(inverseViewProjection, 0.0f, 0.0f, 1.0f, farPoint) ||
      ember::lengthSq(farPoint - nearPoint) <= ember::kEpsilon) {
    return false;
  }
  for (float y : {-1.0f, 1.0f}) {
    for (float x : {-1.0f, 1.0f}) {
      ember::Vec3 cornerNear, cornerFar;
      if (!unproject(inverseViewProjection, x, y, -1.0f, cornerNear) ||
          !unproject(inverseViewProjection, x, y, 1.0f, cornerFar) ||
          ember::lengthSq(cornerFar - cornerNear) <= ember::kEpsilon) {
        return false;
      }
    }
  }

  std::vector<Triangle> triangles;
  std::vector<std::size_t> bvhTriangleIndices;
  std::vector<BvhNode> bvhNodes;
  std::vector<ember::LightInstance> lights;
  std::vector<TextureImage> materialTextures;
  std::unordered_map<std::string, std::size_t> textureIndices;
  const auto acquireTexture = [&materialTextures, &textureIndices, &packet](
      const ember::ImageTextureRef& reference) {
    if (reference.path.empty()) return kNoTexture;
    const std::string key = reference.path + "\n" +
        std::to_string(static_cast<unsigned int>(reference.colorSpace));
    const auto found = textureIndices.find(key);
    if (found != textureIndices.end()) return found->second;
    const std::optional<TextureImage> loaded = loadTextureImage(
        reference.path, packet.assetBaseDirectory, reference.colorSpace);
    if (!loaded) {
      textureIndices.emplace(key, kNoTexture);
      return kNoTexture;
    }
    const std::size_t index = materialTextures.size();
    materialTextures.push_back(*loaded);
    textureIndices.emplace(key, index);
    return index;
  };
  for (const ember::LightInstance& source : packet.lights) {
    if (source.type != ember::LightType::Directional && source.type != ember::LightType::Point) {
      continue;
    }
    ember::LightInstance light = source;
    if (light.type == ember::LightType::Point && !finite(light.position)) continue;
    light.direction = finite(light.direction) ? ember::normalize(light.direction) : ember::Vec3{};
    light.color = ember::Color{boundedNonnegative(light.color.rgb()), 1.0f};
    light.intensity = nonnegativeFinite(light.intensity);
    light.radius = nonnegativeFinite(light.radius);
    lights.push_back(light);
  }

  for (const ember::DrawItem& item : packet.items) {
    const ember::MeshData* mesh = item.mesh;
    if (mesh == nullptr || mesh->positions.size() < 9 || mesh->indices.size() < 3) continue;
    const std::size_t vertexCount = mesh->positions.size() / 3;
    const bool hasNormals = mesh->normals.size() >= vertexCount * 3;
    const ember::Mat4 normalsMatrix = ember::normalMatrix(item.model);

    for (std::size_t index = 0; index + 2 < mesh->indices.size(); index += 3) {
      const ember::u32 i0 = mesh->indices[index + 0];
      const ember::u32 i1 = mesh->indices[index + 1];
      const ember::u32 i2 = mesh->indices[index + 2];
      if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;

      const auto positionAt = [mesh, &item](ember::u32 vertex) {
        const std::size_t offset = static_cast<std::size_t>(vertex) * 3;
        return ember::transformPoint(item.model,
                                     {mesh->positions[offset], mesh->positions[offset + 1],
                                      mesh->positions[offset + 2]});
      };

      Triangle triangle{};
      triangle.p0 = positionAt(i0);
      triangle.p1 = positionAt(i1);
      triangle.p2 = positionAt(i2);
      if (!finite(triangle.p0) || !finite(triangle.p1) || !finite(triangle.p2)) continue;
      const ember::Vec3 geometricNormal = ember::normalize(ember::cross(triangle.p1 - triangle.p0,
                                                                        triangle.p2 - triangle.p0));
      if (ember::lengthSq(geometricNormal) <= ember::kEpsilon) continue;

      const auto normalAt = [mesh, &normalsMatrix, hasNormals, geometricNormal](ember::u32 vertex) {
        if (!hasNormals) return geometricNormal;
        const std::size_t offset = static_cast<std::size_t>(vertex) * 3;
        const ember::Vec3 local{mesh->normals[offset], mesh->normals[offset + 1], mesh->normals[offset + 2]};
        if (!finite(local)) return geometricNormal;
        const ember::Vec3 world = ember::normalize(ember::transformDirection(normalsMatrix, local));
        return ember::lengthSq(world) > ember::kEpsilon ? world : geometricNormal;
      };
      triangle.n0 = normalAt(i0);
      triangle.n1 = normalAt(i1);
      triangle.n2 = normalAt(i2);
      const bool hasUvs = mesh->uvs.size() >= vertexCount * 2;
      const auto uvAt = [mesh](ember::u32 vertex) {
        const std::size_t offset = static_cast<std::size_t>(vertex) * 2;
        return ember::Vec2{mesh->uvs[offset], mesh->uvs[offset + 1]};
      };
      if (hasUvs) {
        triangle.uv0 = uvAt(i0);
        triangle.uv1 = uvAt(i1);
        triangle.uv2 = uvAt(i2);
        triangle.hasUvs = finite(triangle.uv0.x) && finite(triangle.uv0.y) &&
                          finite(triangle.uv1.x) && finite(triangle.uv1.y) &&
                          finite(triangle.uv2.x) && finite(triangle.uv2.y);
      }

      triangle.boundsMin = {
          ember::minf(triangle.p0.x, ember::minf(triangle.p1.x, triangle.p2.x)),
          ember::minf(triangle.p0.y, ember::minf(triangle.p1.y, triangle.p2.y)),
          ember::minf(triangle.p0.z, ember::minf(triangle.p1.z, triangle.p2.z))};
      triangle.boundsMax = {
          ember::maxf(triangle.p0.x, ember::maxf(triangle.p1.x, triangle.p2.x)),
          ember::maxf(triangle.p0.y, ember::maxf(triangle.p1.y, triangle.p2.y)),
          ember::maxf(triangle.p0.z, ember::maxf(triangle.p1.z, triangle.p2.z))};
      // Accumulate in double so finite, large coordinates neither overflow nor
      // produce a NaN that would violate the BVH sort comparator's ordering.
      triangle.centroid = {
          static_cast<float>((static_cast<double>(triangle.p0.x) + triangle.p1.x + triangle.p2.x) / 3.0),
          static_cast<float>((static_cast<double>(triangle.p0.y) + triangle.p1.y + triangle.p2.y) / 3.0),
          static_cast<float>((static_cast<double>(triangle.p0.z) + triangle.p1.z + triangle.p2.z) / 3.0)};

      triangle.material = item.material;
      if (item.materials != nullptr && item.materialCount > 0) {
        triangle.material = item.materials[0].material;
        if (item.triangleMaterialSlots != nullptr &&
            item.triangleMaterialSlotCount == mesh->indices.size() / 3) {
          const ember::u32 slot = item.triangleMaterialSlots[index / 3];
          if (slot < item.materialCount) triangle.material = item.materials[slot].material;
        }
      }
      triangle.material = sanitizedMaterial(triangle.material);
      triangle.textureIndex = acquireTexture(triangle.material.baseColorTexture);
      triangles.push_back(triangle);
    }
  }

  std::vector<Emitter> emitters;
  std::vector<double> triangleEmitterAreas(triangles.size(), 0.0);
  double totalEmitterArea = 0.0;
  for (std::size_t i = 0; i < triangles.size(); ++i) {
    const Triangle& triangle = triangles[i];
    const ember::Color& color = triangle.material.emissionColor;
    if (!(triangle.material.emissionStrength > 0.0f) ||
        !(color.r > 0.0f || color.g > 0.0f || color.b > 0.0f)) {
      continue;
    }
    const double area = triangleArea(triangle.p0, triangle.p1, triangle.p2);
    if (!(area > 0.0) || !std::isfinite(totalEmitterArea + area)) continue;
    totalEmitterArea += area;
    triangleEmitterAreas[i] = area;
    emitters.push_back({i, totalEmitterArea});
  }

  // Deterministic median-split BVH: stable triangle order is the tie-break for
  // equal centroids, and fixed-size leaves bound traversal work.
  constexpr std::size_t kBvhLeafSize = 4;
  bvhTriangleIndices.resize(triangles.size());
  for (std::size_t i = 0; i < bvhTriangleIndices.size(); ++i) bvhTriangleIndices[i] = i;
  bvhNodes.reserve(triangles.empty() ? 0 : triangles.size() * 2);
  const auto buildNode = [&triangles, &bvhTriangleIndices, &bvhNodes](auto&& self,
      std::size_t first, std::size_t count) -> std::size_t {
    const std::size_t nodeIndex = bvhNodes.size();
    bvhNodes.emplace_back();
    ember::Vec3 boundsMin{std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::infinity()};
    ember::Vec3 boundsMax{-std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()};
    ember::Vec3 centroidMin = boundsMin;
    ember::Vec3 centroidMax = boundsMax;
    for (std::size_t offset = first; offset < first + count; ++offset) {
      const Triangle& triangle = triangles[bvhTriangleIndices[offset]];
      boundsMin.x = ember::minf(boundsMin.x, triangle.boundsMin.x);
      boundsMin.y = ember::minf(boundsMin.y, triangle.boundsMin.y);
      boundsMin.z = ember::minf(boundsMin.z, triangle.boundsMin.z);
      boundsMax.x = ember::maxf(boundsMax.x, triangle.boundsMax.x);
      boundsMax.y = ember::maxf(boundsMax.y, triangle.boundsMax.y);
      boundsMax.z = ember::maxf(boundsMax.z, triangle.boundsMax.z);
      centroidMin.x = ember::minf(centroidMin.x, triangle.centroid.x);
      centroidMin.y = ember::minf(centroidMin.y, triangle.centroid.y);
      centroidMin.z = ember::minf(centroidMin.z, triangle.centroid.z);
      centroidMax.x = ember::maxf(centroidMax.x, triangle.centroid.x);
      centroidMax.y = ember::maxf(centroidMax.y, triangle.centroid.y);
      centroidMax.z = ember::maxf(centroidMax.z, triangle.centroid.z);
    }
    bvhNodes[nodeIndex].boundsMin = boundsMin;
    bvhNodes[nodeIndex].boundsMax = boundsMax;
    if (count <= kBvhLeafSize) {
      bvhNodes[nodeIndex].first = first;
      bvhNodes[nodeIndex].count = count;
      return nodeIndex;
    }

    const double extentX = static_cast<double>(centroidMax.x) - centroidMin.x;
    const double extentY = static_cast<double>(centroidMax.y) - centroidMin.y;
    const double extentZ = static_cast<double>(centroidMax.z) - centroidMin.z;
    int axis = 0;
    if (extentY > extentX) axis = 1;
    if (extentZ > (axis == 0 ? extentX : extentY)) axis = 2;
    const std::size_t middle = first + count / 2;
    std::sort(bvhTriangleIndices.begin() + static_cast<std::ptrdiff_t>(first),
              bvhTriangleIndices.begin() + static_cast<std::ptrdiff_t>(first + count),
              [&triangles, axis](std::size_t lhs, std::size_t rhs) {
                const float a = component(triangles[lhs].centroid, axis);
                const float b = component(triangles[rhs].centroid, axis);
                return a == b ? lhs < rhs : a < b;
              });
    const std::size_t left = self(self, first, middle - first);
    const std::size_t right = self(self, middle, first + count - middle);
    bvhNodes[nodeIndex].left = left;
    bvhNodes[nodeIndex].right = right;
    return nodeIndex;
  };
  if (!triangles.empty()) buildNode(buildNode, 0, triangles.size());

  const ember::Color& sourceBackground = packet.background.color;
  const ember::Vec3 background = boundedNonnegative(sourceBackground.rgb());
  const float backgroundStrength = nonnegativeFinite(packet.background.strength, 1.0f);
  const bool perspective = std::fabs(packet.projection.m[15]) < 0.5f;
  TextureImage environmentTexture;
  std::vector<EnvironmentTexel> environmentTexels;
  double environmentWeight = 0.0;
  if (!packet.background.environmentTexture.path.empty()) {
    const std::optional<TextureImage> loaded = loadTextureImage(
        packet.background.environmentTexture.path, packet.assetBaseDirectory,
        packet.background.environmentTexture.colorSpace);
    if (loaded) {
      environmentTexture = *loaded;
      const double dPhi = 2.0 * static_cast<double>(ember::kPi) / environmentTexture.width;
      environmentTexels.resize(static_cast<std::size_t>(environmentTexture.width) *
                               environmentTexture.height);
      for (int y = 0; y < environmentTexture.height; ++y) {
        const double theta0 = static_cast<double>(ember::kPi) * y / environmentTexture.height;
        const double theta1 = static_cast<double>(ember::kPi) * (y + 1) / environmentTexture.height;
        const double solidAngle = dPhi * (std::cos(theta0) - std::cos(theta1));
        for (int x = 0; x < environmentTexture.width; ++x) {
          const std::size_t index = static_cast<std::size_t>(y) * environmentTexture.width + x;
          const double weight = static_cast<double>(luminance(environmentTexture.linearRgba[index])) *
                                solidAngle;
          environmentWeight += weight;
          environmentTexels[index] = {environmentWeight, weight, solidAngle};
        }
      }
      if (environmentWeight > 0.0 && std::isfinite(environmentWeight)) {
        for (EnvironmentTexel& texel : environmentTexels)
          texel.probability /= environmentWeight;
      } else {
        environmentWeight = 0.0;
        environmentTexels.clear();
      }
    }
  }

  triangles_ = std::move(triangles);
  emitters_ = std::move(emitters);
  triangleEmitterAreas_ = std::move(triangleEmitterAreas);
  totalEmitterArea_ = totalEmitterArea;
  bvhTriangleIndices_ = std::move(bvhTriangleIndices);
  bvhNodes_ = std::move(bvhNodes);
  lights_ = std::move(lights);
  materialTextures_ = std::move(materialTextures);
  environmentTexture_ = std::move(environmentTexture);
  environmentTexels_ = std::move(environmentTexels);
  environmentWeight_ = environmentWeight;
  inverseViewProjection_ = inverseViewProjection;
  cameraPosition_ = packet.cameraPosition;
  backgroundColor_ = ember::Color{background, 1.0f};
  backgroundStrength_ = backgroundStrength;
  backgroundLighting_ = packet.background.lighting;
  perspectiveCamera_ = perspective;
  settings_ = settings;
  image_.width = width;
  image_.height = height;
  image_.samplesPerPixel = 0;
  const std::size_t pixelCount = static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height);
  image_.linearPixels.assign(pixelCount, ember::Color{0.0f, 0.0f, 0.0f, 1.0f});
  image_.pixelSampleCounts.assign(pixelCount, 0);
  accumulatedRadiance_.assign(image_.linearPixels.size(), ember::Vec3{});
  if (!adaptiveSampler_.reset(pixelCount)) return false;
  sceneReady_ = true;
  return true;
}

bool PathTracer::intersect(const ember::Ray& ray, float maxDistance, Hit& hit) const {
  if (bvhNodes_.empty() || !finite(ray.origin) || !finite(ray.direction) ||
      !(maxDistance > kRayEpsilon)) {
    return false;
  }

  struct PendingNode {
    std::size_t index;
    float nearDistance;
  };
  // Median splitting bounds tree depth logarithmically; even the maximum
  // addressable triangle count fits comfortably in this explicit traversal stack.
  std::array<PendingNode, 128> stack{};
  std::size_t stackSize = 0;
  float rootNear = 0.0f;
  if (!intersectBounds(bvhNodes_[0].boundsMin, bvhNodes_[0].boundsMax,
                       ray, maxDistance, rootNear)) {
    return false;
  }
  stack[stackSize++] = {0, rootNear};
  bool found = false;
  while (stackSize > 0) {
    const PendingNode pending = stack[--stackSize];
    if (pending.nearDistance > hit.distance || pending.nearDistance > maxDistance) continue;
    const BvhNode& node = bvhNodes_[pending.index];
    if (node.count > 0) {
      const std::size_t end = node.first + node.count;
      for (std::size_t offset = node.first; offset < end; ++offset) {
        const std::size_t triangleIndex = bvhTriangleIndices_[offset];
        const Triangle& triangle = triangles_[triangleIndex];
        const ember::Vec3 edge1 = triangle.p1 - triangle.p0;
        const ember::Vec3 edge2 = triangle.p2 - triangle.p0;
        const ember::Vec3 p = ember::cross(ray.direction, edge2);
        const float determinant = ember::dot(edge1, p);
        if (!finite(determinant) || std::fabs(determinant) <= 1.0e-8f) continue;
        const float inverseDeterminant = 1.0f / determinant;
        const ember::Vec3 offsetFromVertex = ray.origin - triangle.p0;
        const float u = ember::dot(offsetFromVertex, p) * inverseDeterminant;
        if (!finite(u) || u < 0.0f || u > 1.0f) continue;
        const ember::Vec3 q = ember::cross(offsetFromVertex, edge1);
        const float v = ember::dot(ray.direction, q) * inverseDeterminant;
        if (!finite(v) || v < 0.0f || u + v > 1.0f) continue;
        const float distance = ember::dot(edge2, q) * inverseDeterminant;
        if (!finite(distance) || distance <= kRayEpsilon || distance >= maxDistance ||
            distance > hit.distance ||
            (distance == hit.distance && triangleIndex >= hit.triangleIndex)) {
          continue;
        }
        hit.distance = distance;
        hit.u = u;
        hit.v = v;
        hit.triangleIndex = triangleIndex;
        found = true;
      }
      continue;
    }

    float leftNear = 0.0f;
    float rightNear = 0.0f;
    const BvhNode& left = bvhNodes_[node.left];
    const BvhNode& right = bvhNodes_[node.right];
    const bool hitLeft = intersectBounds(left.boundsMin, left.boundsMax,
                                         ray, ember::minf(maxDistance, hit.distance), leftNear);
    const bool hitRight = intersectBounds(right.boundsMin, right.boundsMax,
                                          ray, ember::minf(maxDistance, hit.distance), rightNear);
    if (hitLeft && hitRight) {
      const PendingNode nearNode = leftNear <= rightNear
                                       ? PendingNode{node.left, leftNear}
                                       : PendingNode{node.right, rightNear};
      const PendingNode farNode = leftNear <= rightNear
                                      ? PendingNode{node.right, rightNear}
                                      : PendingNode{node.left, leftNear};
      stack[stackSize++] = farNode;
      stack[stackSize++] = nearNode;
    } else if (hitLeft) {
      stack[stackSize++] = {node.left, leftNear};
    } else if (hitRight) {
      stack[stackSize++] = {node.right, rightNear};
    }
  }
  return found;
}

void PathTracer::reset() {
  image_.samplesPerPixel = 0;
  std::fill(image_.pixelSampleCounts.begin(), image_.pixelSampleCounts.end(), 0);
  (void)adaptiveSampler_.reset(image_.pixelSampleCounts.size());
  for (ember::Color& pixel : image_.linearPixels) {
    pixel = ember::Color{0.0f, 0.0f, 0.0f, 1.0f};
  }
  for (ember::Vec3& sum : accumulatedRadiance_) sum = ember::Vec3{};
}

bool PathTracer::renderSamples(ember::u32 sampleCount) {
  return renderSamplesImpl(sampleCount, nullptr);
}

bool PathTracer::renderAdaptiveSample(const PathAdaptiveSamplingSettings& settings) {
  if (!std::isfinite(settings.absoluteError) || settings.absoluteError < 0.0 ||
      !std::isfinite(settings.relativeError) || settings.relativeError < 0.0 ||
      settings.minSamples == 0 || settings.maxSamples < settings.minSamples ||
      settings.maxSamples > kMaxSamplesPerPixel) {
    return false;
  }
  return renderSamplesImpl(1, &settings);
}

bool PathTracer::adaptiveComplete(const PathAdaptiveSamplingSettings& settings) const {
  if (!sceneReady_ || !std::isfinite(settings.absoluteError) || settings.absoluteError < 0.0 ||
      !std::isfinite(settings.relativeError) || settings.relativeError < 0.0 ||
      settings.minSamples == 0 || settings.maxSamples < settings.minSamples ||
      settings.maxSamples > kMaxSamplesPerPixel || image_.pixelSampleCounts.empty()) {
    return false;
  }
  for (std::size_t pixel = 0; pixel < image_.pixelSampleCounts.size(); ++pixel) {
    if (adaptiveSampler_.needsSample(pixel, settings)) return false;
  }
  return true;
}

bool PathTracer::renderSamplesImpl(
    ember::u32 sampleCount, const PathAdaptiveSamplingSettings* adaptiveSettings) {
  if (!sceneReady_ || sampleCount == 0 || sampleCount > kMaxSamplesPerBatch) return false;
  if (adaptiveSettings == nullptr &&
      sampleCount > kMaxSamplesPerPixel - image_.samplesPerPixel) return false;
  if (adaptiveSettings != nullptr && sampleCount != 1) return false;
  for (ember::u32 count : image_.pixelSampleCounts) {
    if (count > kMaxSamplesPerPixel - sampleCount && adaptiveSettings == nullptr) return false;
    if (adaptiveSettings == nullptr && count != image_.samplesPerPixel) return false;
  }

  const auto getSurfaceNormal = [this](const Hit& hit) {
    const Triangle& triangle = triangles_[hit.triangleIndex];
    const float w = 1.0f - hit.u - hit.v;
    ember::Vec3 normal = ember::normalize(triangle.n0 * w + triangle.n1 * hit.u + triangle.n2 * hit.v);
    const ember::Vec3 geometric = ember::normalize(ember::cross(triangle.p1 - triangle.p0,
                                                                triangle.p2 - triangle.p0));
    if (ember::lengthSq(normal) <= ember::kEpsilon) {
      normal = geometric;
    }
    // Keep the authored/outward orientation: the BSDF uses the sign of the
    // outgoing direction to distinguish entering from exiting dielectric paths.
    return ember::dot(normal, geometric) < 0.0f ? -normal : normal;
  };

  const auto environmentRadiance = [this](ember::Vec3 direction) {
    if (environmentTexture_.width > 0 && environmentTexture_.height > 0) {
      const ember::Color sample = sampleTextureImage(environmentTexture_,
                                                       environmentUv(direction), true);
      return boundedNonnegative(sample.rgb()) * backgroundStrength_;
    }
    return boundedNonnegative(backgroundColor_.rgb()) * backgroundStrength_;
  };

  const auto environmentPdf = [this](ember::Vec3 direction) {
    if (environmentTexels_.empty() || !(environmentWeight_ > 0.0))
      return 1.0f / (4.0f * ember::kPi);
    const ember::Vec2 uv = environmentUv(direction);
    const int x = std::clamp(static_cast<int>(uv.x * environmentTexture_.width),
                             0, environmentTexture_.width - 1);
    const int y = std::clamp(static_cast<int>((1.0f - uv.y) * environmentTexture_.height),
                             0, environmentTexture_.height - 1);
    const EnvironmentTexel& texel = environmentTexels_[
        static_cast<std::size_t>(y) * environmentTexture_.width + x];
    if (!(texel.solidAngle > 0.0) || !(texel.probability > 0.0)) return 0.0f;
    return static_cast<float>(texel.probability / texel.solidAngle);
  };

  const auto sampleEnvironment = [this](Random& random, ember::Vec3& direction,
                                         float& pdf) {
    if (environmentTexels_.empty() || !(environmentWeight_ > 0.0)) {
      const float z = 1.0f - 2.0f * random.nextFloat();
      const float phi = ember::kTwoPi * random.nextFloat();
      const float radial = std::sqrt(ember::maxf(0.0f, 1.0f - z * z));
      direction = {radial * std::cos(phi), radial * std::sin(phi), z};
      pdf = 1.0f / (4.0f * ember::kPi);
      return;
    }
    const double target = static_cast<double>(random.nextFloat()) * environmentWeight_;
    const auto texelIt = std::lower_bound(environmentTexels_.begin(), environmentTexels_.end(),
        target, [](const EnvironmentTexel& texel, double value) {
          return texel.cumulativeWeight < value;
        });
    const std::size_t index = texelIt == environmentTexels_.end()
                                  ? environmentTexels_.size() - 1
                                  : static_cast<std::size_t>(texelIt - environmentTexels_.begin());
    const int x = static_cast<int>(index % static_cast<std::size_t>(environmentTexture_.width));
    const int y = static_cast<int>(index / static_cast<std::size_t>(environmentTexture_.width));
    const float phi = ember::kTwoPi *
        ((static_cast<float>(x) + random.nextFloat()) / environmentTexture_.width - 0.5f);
    const double cosTheta0 = std::cos(static_cast<double>(ember::kPi) * y /
                                      environmentTexture_.height);
    const double cosTheta1 = std::cos(static_cast<double>(ember::kPi) * (y + 1) /
                                      environmentTexture_.height);
    const float cosTheta = static_cast<float>(cosTheta0 + random.nextFloat() *
                                               (cosTheta1 - cosTheta0));
    const float sinTheta = std::sqrt(ember::maxf(0.0f, 1.0f - cosTheta * cosTheta));
    direction = {sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi)};
    const EnvironmentTexel& texel = environmentTexels_[index];
    pdf = texel.solidAngle > 0.0
              ? static_cast<float>(texel.probability / texel.solidAngle) : 0.0f;
  };

    const auto directLighting = [this, &environmentRadiance, &sampleEnvironment](ember::Vec3 position,
      ember::Vec3 geometricNormal, ember::Vec3 normal, ember::Vec3 outgoing,
      const ember::MaterialDesc& material, Random& random,
      float bsdfContinuationProbability) {
    ember::Vec3 result{};
    for (const ember::LightInstance& light : lights_) {
      ember::Vec3 directionToLight{};
      float maxDistance = std::numeric_limits<float>::infinity();
      float attenuation = 1.0f;
      if (light.type == ember::LightType::Directional) {
        directionToLight = ember::normalize(-light.direction);
        if (ember::lengthSq(directionToLight) <= ember::kEpsilon) continue;
      } else {
        const ember::Vec3 toLight = light.position - position;
        const float distance = ember::length(toLight);
        if (!(distance > kRayEpsilon) || !(light.radius > 0.0f) || distance >= light.radius) continue;
        directionToLight = toLight / distance;
        maxDistance = distance - kRayEpsilon;
        const float falloff = 1.0f - distance / light.radius;
        attenuation = falloff * falloff;
      }

      const float cosine = std::fabs(ember::dot(normal, directionToLight));
      if (cosine <= 0.0f || attenuation <= 0.0f) continue;
      const ember::Vec3 offsetNormal = ember::dot(geometricNormal, directionToLight) >= 0.0f
                                           ? geometricNormal : -geometricNormal;
      const ember::Ray shadowRay{position + offsetNormal * kRayEpsilon, directionToLight};
      Hit blocker{};
      if (intersect(shadowRay, maxDistance, blocker)) continue;

      const ember::Vec3 radiance = light.color.rgb() * (light.intensity * attenuation);
      const PathBsdfEval bsdf = evaluatePathBsdf(material, normal, outgoing, directionToLight);
      result += (bsdf.value * radiance) * cosine;
    }

    // A constant environment uses a uniform-sphere proposal; image environments
    // use their luminance-weighted lat-long texel distribution. Its sample is
    // paired with BSDF continuation using the power heuristic; a primary miss
    // remains an unweighted camera-visible background sample.
    const ember::Vec3 constantEnvironment = backgroundColor_.rgb() * backgroundStrength_;
    const bool hasEnvironmentEnergy = environmentWeight_ > 0.0 ||
        constantEnvironment.x > 0.0f || constantEnvironment.y > 0.0f ||
        constantEnvironment.z > 0.0f;
    if (backgroundLighting_ && hasEnvironmentEnergy) {
      ember::Vec3 direction{};
      float lightPdf = 0.0f;
      sampleEnvironment(random, direction, lightPdf);
      const float cosine = std::fabs(ember::dot(normal, direction));
      if (cosine > 0.0f && lightPdf > 0.0f && finite(lightPdf)) {
        const ember::Vec3 offsetNormal = ember::dot(geometricNormal, direction) >= 0.0f
                                             ? geometricNormal : -geometricNormal;
        Hit blocker{};
        if (!intersect({position + offsetNormal * kRayEpsilon, direction},
                       std::numeric_limits<float>::infinity(), blocker)) {
          const PathBsdfEval bsdf = evaluatePathBsdf(material, normal, outgoing, direction);
          const ember::Vec3 sampledEnvironment = environmentRadiance(direction);
          const float mis = bsdfContinuationProbability > 0.0f
                                ? powerHeuristic(lightPdf,
                                                 bsdf.pdf * bsdfContinuationProbability)
                                : 1.0f;
          result += (bsdf.value * sampledEnvironment) * (cosine * mis / lightPdf);
        }
      }
    }

    // Emissive triangles are sampled uniformly over their combined area. The
    // area PDF is converted to solid angle at the shading point; the matching
    // BSDF-hit strategy is weighted in trace() below.
    if (totalEmitterArea_ > 0.0 && !emitters_.empty()) {
      const double targetArea = static_cast<double>(random.nextFloat()) * totalEmitterArea_;
      const auto emitterIt = std::lower_bound(emitters_.begin(), emitters_.end(), targetArea,
          [](const Emitter& emitter, double target) { return emitter.cumulativeArea < target; });
      const Emitter& emitter = emitterIt == emitters_.end() ? emitters_.back() : *emitterIt;
      const Triangle& lightTriangle = triangles_[emitter.triangleIndex];
      const float root = std::sqrt(random.nextFloat());
      const float bary = random.nextFloat();
      const float b0 = 1.0f - root;
      const float b1 = root * (1.0f - bary);
      const float b2 = root * bary;
      const ember::Vec3 lightPoint = lightTriangle.p0 * b0 + lightTriangle.p1 * b1 +
                                     lightTriangle.p2 * b2;
      const ember::Vec3 toLight = lightPoint - position;
      const float distanceSq = ember::lengthSq(toLight);
      if (distanceSq > kRayEpsilon * kRayEpsilon && finite(distanceSq)) {
        const float distance = std::sqrt(distanceSq);
        const ember::Vec3 direction = toLight / distance;
        const ember::Vec3 lightGeometric = ember::normalize(
            ember::cross(lightTriangle.p1 - lightTriangle.p0,
                         lightTriangle.p2 - lightTriangle.p0));
        const float lightCosine = std::fabs(ember::dot(lightGeometric, -direction));
        const float surfaceCosine = std::fabs(ember::dot(normal, direction));
        if (lightCosine > 1.0e-8f && surfaceCosine > 0.0f) {
          const float lightPdf = static_cast<float>(distanceSq /
              (totalEmitterArea_ * static_cast<double>(lightCosine)));
          const ember::Vec3 offsetNormal = ember::dot(geometricNormal, direction) >= 0.0f
                                               ? geometricNormal : -geometricNormal;
          Hit blocker{};
          const bool blocked = intersect({position + offsetNormal * kRayEpsilon, direction},
                                          distance - kRayEpsilon, blocker);
          if (lightPdf > 0.0f && finite(lightPdf) && !blocked) {
            const PathBsdfEval bsdf = evaluatePathBsdf(material, normal, outgoing, direction);
            const ember::Color& emission = lightTriangle.material.emissionColor;
            const float strength = emissionComponent(lightTriangle.material.emissionStrength);
            const ember::Vec3 emitted{
                boundedEmissionProduct(emissionComponent(emission.r), strength),
                boundedEmissionProduct(emissionComponent(emission.g), strength),
                boundedEmissionProduct(emissionComponent(emission.b), strength)};
            const float mis = bsdfContinuationProbability > 0.0f
                                  ? powerHeuristic(lightPdf,
                                                   bsdf.pdf * bsdfContinuationProbability)
                                  : 1.0f;
            result += (bsdf.value * clampRadiance(emitted)) *
                      (surfaceCosine * mis / lightPdf);
          }
        }
      }
    }
    return clampRadiance(result);
  };

  const auto trace = [this, &getSurfaceNormal, &directLighting, &environmentRadiance,
                      &environmentPdf](ember::Ray ray, Random& random) {
    ember::Vec3 radiance{};
    ember::Vec3 throughput{1.0f, 1.0f, 1.0f};
    ember::Vec3 previousPosition{};
    float previousBsdfPdf = 0.0f;
    bool previousDelta = true;
    for (ember::u32 bounce = 0; bounce <= settings_.maxBounces; ++bounce) {
      Hit hit{};
      if (!intersect(ray, std::numeric_limits<float>::infinity(), hit)) {
        if (bounce == 0 || backgroundLighting_) {
          const ember::Vec3 environment = environmentRadiance(ray.direction);
          float mis = 1.0f;
          if (bounce > 0 && !previousDelta) {
            mis = powerHeuristic(previousBsdfPdf, environmentPdf(ray.direction));
          }
          radiance += throughput * environment * mis;
        }
        break;
      }

      const Triangle& triangle = triangles_[hit.triangleIndex];
      const ember::Vec3 position = ray.pointAt(hit.distance);
      const ember::Vec3 normal = getSurfaceNormal(hit);
      const ember::Vec3 geometricNormal = ember::normalize(
          ember::cross(triangle.p1 - triangle.p0, triangle.p2 - triangle.p0));
      const ember::Vec3 emissionColor = triangle.material.emissionColor.rgb();
      const float emissionStrength = emissionComponent(triangle.material.emissionStrength);
      const ember::Vec3 emitted{
          boundedEmissionProduct(emissionComponent(emissionColor.x), emissionStrength),
           boundedEmissionProduct(emissionComponent(emissionColor.y), emissionStrength),
           boundedEmissionProduct(emissionComponent(emissionColor.z), emissionStrength)};
      float emissionMis = 1.0f;
      if (bounce > 0 && !previousDelta && totalEmitterArea_ > 0.0 &&
          hit.triangleIndex < triangleEmitterAreas_.size() &&
          triangleEmitterAreas_[hit.triangleIndex] > 0.0) {
        const ember::Vec3 fromPrevious = position - previousPosition;
        const float distanceSq = ember::lengthSq(fromPrevious);
        const ember::Vec3 direction = ember::normalize(fromPrevious);
        const float lightCosine = std::fabs(ember::dot(geometricNormal, -direction));
        if (distanceSq > 0.0f && lightCosine > 1.0e-8f) {
          const float lightPdf = static_cast<float>(distanceSq /
              (totalEmitterArea_ * static_cast<double>(lightCosine)));
          emissionMis = powerHeuristic(previousBsdfPdf, lightPdf);
        }
      }
      radiance += throughput * clampRadiance(emitted) * emissionMis;
      ember::MaterialDesc material = triangle.material;
      if (triangle.hasUvs && triangle.textureIndex < materialTextures_.size()) {
        const float bary0 = 1.0f - hit.u - hit.v;
        const ember::Vec2 uv{
            triangle.uv0.x * bary0 + triangle.uv1.x * hit.u + triangle.uv2.x * hit.v,
            triangle.uv0.y * bary0 + triangle.uv1.y * hit.u + triangle.uv2.y * hit.v};
        const ember::Color texel = sampleTextureImage(
            materialTextures_[triangle.textureIndex], uv, true);
        material.baseColor.r *= nonnegativeFinite(texel.r);
        material.baseColor.g *= nonnegativeFinite(texel.g);
        material.baseColor.b *= nonnegativeFinite(texel.b);
      }
      const ember::Vec3 outgoing = -ray.direction;
      float continuationSurvival = 0.0f;
      if (bounce < settings_.maxBounces) {
        continuationSurvival = 1.0f;
        if (bounce >= settings_.rouletteStartBounce) {
          continuationSurvival = ember::clampf(
              ember::maxf(throughput.x, ember::maxf(throughput.y, throughput.z)),
              0.05f, 0.95f);
        }
      }
      radiance += throughput * directLighting(position, geometricNormal, normal,
                                                outgoing, material, random,
                                                continuationSurvival);
      // The bounce limit stops continuation, not direct illumination at the
      // terminal surface. A zero-bounce render still evaluates primary-hit NEE.
      if (bounce == settings_.maxBounces) break;

      const float lobeSample = random.nextFloat();
      const float sampleU1 = random.nextFloat();
      const float sampleU2 = random.nextFloat();
      const PathBsdfSample sample = samplePathBsdf(material, normal, outgoing,
                                                    lobeSample, sampleU1, sampleU2);
      if (!sample.valid || !finite(sample.direction) || !finite(sample.weight)) break;
      throughput = throughput * sample.weight;
      if (throughput.x <= 0.0f && throughput.y <= 0.0f && throughput.z <= 0.0f) break;
      // Roulette probability is chosen from the incoming throughput, before
      // sampling direction, so it is independent of the BSDF proposal. Include
      // that survival probability in both sides of the subsequent MIS pair.
      if (continuationSurvival < 1.0f) {
        if (random.nextFloat() >= continuationSurvival) break;
        throughput = throughput / continuationSurvival;
      }
      previousPosition = position;
      previousBsdfPdf = sample.pdf * continuationSurvival;
      previousDelta = sample.delta;
      const ember::Vec3 offsetNormal = ember::dot(geometricNormal, sample.direction) >= 0.0f
                                           ? geometricNormal : -geometricNormal;
      ray.origin = position + offsetNormal * kRayEpsilon;
      ray.direction = sample.direction;
    }
    return clampRadiance(radiance);
  };

  const float width = static_cast<float>(image_.width);
  const float height = static_cast<float>(image_.height);
  const std::size_t pixelCount = image_.linearPixels.size();
  for (int y = 0; y < image_.height; ++y) {
    for (int x = 0; x < image_.width; ++x) {
      const std::size_t pixelIndex = static_cast<std::size_t>(y) * image_.width + x;
      if (adaptiveSettings != nullptr &&
          !adaptiveSampler_.needsSample(pixelIndex, *adaptiveSettings)) continue;
      const ember::u32 scheduledSamples = adaptiveSettings == nullptr ? sampleCount : 1u;
      for (ember::u32 sampleOffset = 0; sampleOffset < scheduledSamples; ++sampleOffset) {
        const ember::u32 sampleIndex = image_.pixelSampleCounts[pixelIndex] + sampleOffset;
        ember::u32 key = hash32(settings_.seed ^ hash32(static_cast<ember::u32>(pixelIndex)) ^
                                hash32(sampleIndex + 0x68bc21ebu));
        Random random{key};
        const float jitterX = random.nextFloat();
        const float jitterY = random.nextFloat();
        const float ndcX = 2.0f * ((static_cast<float>(x) + jitterX) / width) - 1.0f;
        const float ndcY = 1.0f - 2.0f * ((static_cast<float>(y) + jitterY) / height);
        ember::Vec3 nearPoint, farPoint;
        if (!unproject(inverseViewProjection_, ndcX, ndcY, -1.0f, nearPoint) ||
            !unproject(inverseViewProjection_, ndcX, ndcY, 1.0f, farPoint)) {
          return false;
        }
        ember::Ray ray;
        ray.origin = perspectiveCamera_ ? cameraPosition_ : nearPoint;
        ray.direction = ember::normalize(farPoint - ray.origin);
        if (ember::lengthSq(ray.direction) <= ember::kEpsilon) return false;
        // Clamp each sample in trace(), but never clamp the running sum: doing
        // so biases the progressive mean downward once accumulated energy hits
        // the per-sample radiance ceiling.
        const ember::Vec3 sampleRadiance = trace(ray, random);
        accumulatedRadiance_[pixelIndex] += sampleRadiance;
        if (!adaptiveSampler_.addSample(pixelIndex,
              static_cast<double>(luminance(ember::Color{sampleRadiance, 1.0f})))) {
          return false;
        }
      }
      image_.pixelSampleCounts[pixelIndex] += scheduledSamples;
    }
  }

  ember::u32 minimumSamples = std::numeric_limits<ember::u32>::max();
  for (std::size_t i = 0; i < pixelCount; ++i) {
    const ember::u32 pixelSamples = image_.pixelSampleCounts[i];
    minimumSamples = std::min(minimumSamples, pixelSamples);
    const ember::Vec3 average = pixelSamples > 0
        ? accumulatedRadiance_[i] * (1.0f / static_cast<float>(pixelSamples))
        : ember::Vec3{};
    image_.linearPixels[i] = ember::Color{average, 1.0f};
  }
  image_.samplesPerPixel = pixelCount == 0 ? 0 : minimumSamples;
  return true;
}

const PathTraceImage& PathTracer::image() const {
  return image_;
}

} // namespace ember::render
