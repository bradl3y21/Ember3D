#include "render/PathDenoiser.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace ember::render {

namespace {

constexpr int kMaxDimension = 8192;
constexpr std::size_t kMaxPixelCount = 16u * 1024u * 1024u;
constexpr int kMaxRadius = 8;

bool finite(float value) {
  return std::isfinite(value);
}

bool validImage(const PathTraceImage& image, std::size_t& pixelCount) {
  if (image.width <= 0 || image.height <= 0 || image.width > kMaxDimension ||
      image.height > kMaxDimension) {
    return false;
  }

  const std::size_t width = static_cast<std::size_t>(image.width);
  const std::size_t height = static_cast<std::size_t>(image.height);
  pixelCount = width * height;
  if (pixelCount > kMaxPixelCount || image.linearPixels.size() != pixelCount) return false;

  for (const ember::Color& pixel : image.linearPixels) {
    if (!finite(pixel.r) || !finite(pixel.g) || !finite(pixel.b) || !finite(pixel.a)) return false;
  }
  return true;
}

bool validSettings(const PathDenoiserSettings& settings) {
  return settings.radius >= 0 && settings.radius <= kMaxRadius &&
         finite(settings.spatialSigma) && settings.spatialSigma > 0.0f &&
         finite(settings.colorSigma) && settings.colorSigma > 0.0f;
}

bool sameRgb(ember::Color a, ember::Color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

} // namespace

bool denoisePathTraceImage(const PathTraceImage& input, PathTraceImage& output,
                           const PathDenoiserSettings& settings) {
  std::size_t pixelCount = 0;
  if (!validImage(input, pixelCount) || !validSettings(settings)) return false;

  PathTraceImage result;
  result.width = input.width;
  result.height = input.height;
  result.samplesPerPixel = input.samplesPerPixel;
  result.linearPixels.resize(pixelCount);

  // Besides avoiding needless work, this makes constant RGB images exactly
  // invariant rather than merely invariant up to floating-point roundoff.
  const ember::Color first = input.linearPixels.front();
  const bool constantRgb = std::all_of(input.linearPixels.begin() + 1,
                                       input.linearPixels.end(),
                                       [first](ember::Color pixel) {
                                         return sameRgb(pixel, first);
                                       });
  if (constantRgb || settings.radius == 0) {
    result.linearPixels = input.linearPixels;
    output = std::move(result);
    return true;
  }

  const double spatialDenominator =
      2.0 * static_cast<double>(settings.spatialSigma) * settings.spatialSigma;
  const double colorDenominator =
      2.0 * static_cast<double>(settings.colorSigma) * settings.colorSigma;
  const int width = input.width;
  const int height = input.height;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t centerIndex = static_cast<std::size_t>(y) * width + x;
      const ember::Color center = input.linearPixels[centerIndex];
      double weightSum = 0.0;
      double red = 0.0;
      double green = 0.0;
      double blue = 0.0;

      const int y0 = std::max(0, y - settings.radius);
      const int y1 = std::min(height - 1, y + settings.radius);
      const int x0 = std::max(0, x - settings.radius);
      const int x1 = std::min(width - 1, x + settings.radius);
      for (int sampleY = y0; sampleY <= y1; ++sampleY) {
        const int dy = sampleY - y;
        for (int sampleX = x0; sampleX <= x1; ++sampleX) {
          const int dx = sampleX - x;
          const ember::Color sample =
              input.linearPixels[static_cast<std::size_t>(sampleY) * width + sampleX];
          const double dr = static_cast<double>(sample.r) - center.r;
          const double dg = static_cast<double>(sample.g) - center.g;
          const double db = static_cast<double>(sample.b) - center.b;
          const double spatialDistance = static_cast<double>(dx * dx + dy * dy);
          const double colorDistance = dr * dr + dg * dg + db * db;
          const double weight = std::exp(-spatialDistance / spatialDenominator -
                                         colorDistance / colorDenominator);
          weightSum += weight;
          red += weight * sample.r;
          green += weight * sample.g;
          blue += weight * sample.b;
        }
      }

      // The center has unit weight, so this guard is defensive only; retaining
      // the center also avoids ever producing a non-finite result.
      if (weightSum > 0.0 && std::isfinite(weightSum)) {
        result.linearPixels[centerIndex] = {
            static_cast<float>(red / weightSum),
            static_cast<float>(green / weightSum),
            static_cast<float>(blue / weightSum), center.a};
      } else {
        result.linearPixels[centerIndex] = center;
      }
    }
  }

  output = std::move(result);
  return true;
}

} // namespace ember::render
