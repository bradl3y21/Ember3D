#include "render/PathSampling.h"

#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace ember::render {

bool PathAdaptiveSampler::reset(std::size_t pixelCount) {
  if (pixelCount == pixels_.size()) {
    for (PixelStatistics& pixel : pixels_) pixel = {};
    return true;
  }
  try {
    std::vector<PixelStatistics> fresh(pixelCount);
    pixels_.swap(fresh);
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  } catch (const std::length_error&) {
    return false;
  }
}

bool PathAdaptiveSampler::validSettings(
    const PathAdaptiveSamplingSettings& settings) {
  return settings.minSamples > 0 &&
         settings.maxSamples >= settings.minSamples &&
         std::isfinite(settings.absoluteError) && settings.absoluteError >= 0.0 &&
         std::isfinite(settings.relativeError) && settings.relativeError >= 0.0;
}

bool PathAdaptiveSampler::hasConverged(
    const PixelStatistics& pixel,
    const PathAdaptiveSamplingSettings& settings) {
  if (pixel.count < settings.minSamples || pixel.count < 2) return false;

  // Welford's M2 is the sum of squared deviations. The unbiased sample
  // variance and standard error are deterministic for the fixed sample order.
  const double variance = pixel.m2 / static_cast<double>(pixel.count - 1);
  const double standardError = std::sqrt(variance / static_cast<double>(pixel.count));
  const double threshold = settings.absoluteError +
                           settings.relativeError * std::fabs(pixel.mean);
  return std::isfinite(standardError) && standardError <= threshold;
}

bool PathAdaptiveSampler::needsSample(
    std::size_t pixel,
    const PathAdaptiveSamplingSettings& settings) const {
  if (!validSettings(settings) || pixel >= pixels_.size()) return false;
  const PixelStatistics& stats = pixels_[pixel];
  if (stats.count >= settings.maxSamples) return false;
  return !hasConverged(stats, settings);
}

bool PathAdaptiveSampler::converged(
    std::size_t pixel,
    const PathAdaptiveSamplingSettings& settings) const {
  if (!validSettings(settings) || pixel >= pixels_.size()) return false;
  return hasConverged(pixels_[pixel], settings);
}

bool PathAdaptiveSampler::allConverged(
    const PathAdaptiveSamplingSettings& settings) const {
  if (!validSettings(settings) || pixels_.empty()) return false;
  for (const PixelStatistics& pixel : pixels_) {
    if (!hasConverged(pixel, settings)) return false;
  }
  return true;
}

bool PathAdaptiveSampler::addSample(std::size_t pixel, double luminance) {
  if (pixel >= pixels_.size() || !std::isfinite(luminance)) return false;

  const PixelStatistics& current = pixels_[pixel];
  if (current.count == std::numeric_limits<std::uint64_t>::max()) return false;

  PixelStatistics updated = current;
  const std::uint64_t nextCount = current.count + 1;
  const double delta = luminance - current.mean;
  const double nextMean = current.mean + delta / static_cast<double>(nextCount);
  const double deltaAfterMean = luminance - nextMean;
  const double nextM2 = current.m2 + delta * deltaAfterMean;

  if (!std::isfinite(delta) || !std::isfinite(nextMean) ||
      !std::isfinite(deltaAfterMean) || !std::isfinite(nextM2)) {
    return false;
  }

  updated.count = nextCount;
  updated.mean = nextMean;
  // Roundoff can make a theoretically zero sum of squares slightly negative.
  updated.m2 = nextM2 < 0.0 ? 0.0 : nextM2;
  pixels_[pixel] = updated;
  return true;
}

std::uint64_t PathAdaptiveSampler::sampleCount(std::size_t pixel) const {
  return pixel < pixels_.size() ? pixels_[pixel].count : 0;
}

} // namespace ember::render
