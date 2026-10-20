#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ember::render {

// Error targets are in scene-linear luminance units. A pixel converges when its
// standard error is no greater than absoluteError + relativeError * |mean|.
struct PathAdaptiveSamplingSettings {
  std::uint64_t minSamples = 4;
  // Per-pixel hard cap. Reaching it stops scheduling even if the error target
  // has not been met.
  std::uint64_t maxSamples = 64;
  double absoluteError = 0.01;
  double relativeError = 0.02;
};

// Deterministic, per-pixel Welford statistics. Samples must be added in the
// same order for reproducible decisions. PathTracer uses these statistics to
// make deterministic per-pixel scheduling decisions.
class PathAdaptiveSampler {
public:
  // Replaces all pixel state. Returns false on allocation/size failure and
  // leaves the previous state unchanged.
  bool reset(std::size_t pixelCount);

  // Invalid settings or an out-of-range pixel fail closed (false).
  bool needsSample(std::size_t pixel,
                   const PathAdaptiveSamplingSettings& settings) const;
  bool converged(std::size_t pixel,
                 const PathAdaptiveSamplingSettings& settings) const;

  // True only when there is at least one pixel and every pixel passes its
  // minimum-sample/error criterion. A sample cap alone is not convergence.
  bool allConverged(const PathAdaptiveSamplingSettings& settings) const;

  // Adds one finite linear-luminance sample. Invalid pixels, non-finite data,
  // and an unrepresentable accumulator update are rejected without mutation.
  bool addSample(std::size_t pixel, double luminance);

  // Returns zero for an out-of-range pixel.
  std::uint64_t sampleCount(std::size_t pixel) const;

private:
  struct PixelStatistics {
    std::uint64_t count = 0;
    double mean = 0.0;
    double m2 = 0.0;
  };

  static bool validSettings(const PathAdaptiveSamplingSettings& settings);
  static bool hasConverged(const PixelStatistics& pixel,
                           const PathAdaptiveSamplingSettings& settings);

  std::vector<PixelStatistics> pixels_;
};

} // namespace ember::render
