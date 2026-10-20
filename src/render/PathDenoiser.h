#pragma once

#include "render/PathTracer.h"

namespace ember::render {

// Parameters for a small CPU bilateral filter over linear RGB. This is a
// deterministic spatial prototype; it is not OIDN or Cycles denoising and it
// does not use path-tracing guides such as normals or albedo.
struct PathDenoiserSettings {
  int radius = 2;
  float spatialSigma = 1.5f;
  float colorSigma = 0.15f;
};

// Denoise an already accumulated linear PathTraceImage into output. On success,
// dimensions, sample count, and alpha are preserved. Input is never modified.
// Returns false for malformed dimensions/pixel storage, non-finite input data,
// or invalid settings; on failure output is left unchanged.
bool denoisePathTraceImage(const PathTraceImage& input, PathTraceImage& output,
                           const PathDenoiserSettings& settings = {});

} // namespace ember::render
