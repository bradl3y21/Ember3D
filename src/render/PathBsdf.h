#pragma once

#include "core/Types.h"

namespace ember::render {

struct PathBsdfEval {
  ember::Vec3 value{};
  float pdf = 0.0f;
};

struct PathBsdfSample {
  ember::Vec3 direction{};
  ember::Vec3 weight{};
  float pdf = 0.0f;
  bool valid = false;
  bool delta = false;
};

// Directions point away from the surface. The evaluator returns the BSDF and
// the sampling density with respect to solid angle. Non-delta sample weights
// are f * abs(dot(normal, direction)) / pdf; delta weights are the sampled
// branch's discrete throughput multiplier.
PathBsdfEval evaluatePathBsdf(const ember::MaterialDesc&, ember::Vec3 normal,
                              ember::Vec3 outgoing, ember::Vec3 incoming);
PathBsdfSample samplePathBsdf(const ember::MaterialDesc&, ember::Vec3 normal,
                              ember::Vec3 outgoing, float lobeSample,
                              float u1, float u2);

} // namespace ember::render
