// CPU-only checks for the deterministic linear-RGB spatial denoising prototype.
#include "harness.h"

#include "render/PathDenoiser.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace ember;
using ember::render::PathDenoiserSettings;
using ember::render::PathTraceImage;
using ember::render::denoisePathTraceImage;

namespace {

bool sameImage(const PathTraceImage& a, const PathTraceImage& b) {
  return a.width == b.width && a.height == b.height &&
         a.samplesPerPixel == b.samplesPerPixel && a.linearPixels == b.linearPixels;
}

PathTraceImage makeNoisyFlatImage() {
  PathTraceImage image;
  image.width = 17;
  image.height = 13;
  image.samplesPerPixel = 9;
  image.linearPixels.reserve(static_cast<std::size_t>(image.width) * image.height);
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      const float noise = ((x * 17 + y * 31) % 2 == 0) ? 0.16f : -0.16f;
      image.linearPixels.emplace_back(0.45f + noise, 0.32f + noise,
                                      0.20f + noise, 0.25f + 0.01f * x);
    }
  }
  return image;
}

double rgbSquaredErrorFrom(const PathTraceImage& image, Vec3 expected) {
  double sum = 0.0;
  for (const Color& pixel : image.linearPixels) {
    const double dr = static_cast<double>(pixel.r) - expected.x;
    const double dg = static_cast<double>(pixel.g) - expected.y;
    const double db = static_cast<double>(pixel.b) - expected.z;
    sum += dr * dr + dg * dg + db * db;
  }
  return sum;
}

} // namespace

TEST(path_denoiser_preserves_constant_linear_image_exactly) {
  PathTraceImage source;
  source.width = 5;
  source.height = 3;
  source.samplesPerPixel = 32;
  source.linearPixels.assign(15, Color{3.25f, 0.75f, 9.0f, 0.4f});
  const PathTraceImage original = source;
  PathTraceImage output;

  CHECK(denoisePathTraceImage(source, output));
  CHECK_EQ(output.width, source.width);
  CHECK_EQ(output.height, source.height);
  CHECK_EQ(output.samplesPerPixel, source.samplesPerPixel);
  CHECK(output.linearPixels == source.linearPixels);
  CHECK(sameImage(source, original));
}

TEST(path_denoiser_reduces_known_spatial_noise_without_changing_alpha) {
  const PathTraceImage source = makeNoisyFlatImage();
  const PathTraceImage original = source;
  PathTraceImage output;
  PathDenoiserSettings settings;
  settings.colorSigma = 0.4f;

  CHECK(denoisePathTraceImage(source, output, settings));
  CHECK_EQ(output.width, source.width);
  CHECK_EQ(output.height, source.height);
  CHECK_EQ(output.samplesPerPixel, source.samplesPerPixel);
  CHECK(rgbSquaredErrorFrom(output, {0.45f, 0.32f, 0.20f}) <
        rgbSquaredErrorFrom(source, {0.45f, 0.32f, 0.20f}) * 0.5);
  for (std::size_t i = 0; i < source.linearPixels.size(); ++i)
    CHECK_EQ(output.linearPixels[i].a, source.linearPixels[i].a);
  CHECK(sameImage(source, original));
}

TEST(path_denoiser_retains_a_high_contrast_edge) {
  PathTraceImage source;
  source.width = 12;
  source.height = 8;
  source.samplesPerPixel = 1;
  source.linearPixels.resize(static_cast<std::size_t>(source.width) * source.height);
  for (int y = 0; y < source.height; ++y) {
    for (int x = 0; x < source.width; ++x) {
      source.linearPixels[static_cast<std::size_t>(y) * source.width + x] =
          x < source.width / 2 ? Color{0.05f, 0.05f, 0.05f, 0.7f}
                               : Color{0.95f, 0.95f, 0.95f, 0.2f};
    }
  }
  PathTraceImage output;

  CHECK(denoisePathTraceImage(source, output));
  for (int y = 0; y < source.height; ++y) {
    const std::size_t left = static_cast<std::size_t>(y) * source.width + 5;
    const std::size_t right = left + 1;
    CHECK(output.linearPixels[left].r < 0.1f);
    CHECK(output.linearPixels[right].r > 0.9f);
    CHECK(output.linearPixels[left].a == source.linearPixels[left].a);
    CHECK(output.linearPixels[right].a == source.linearPixels[right].a);
  }
}

TEST(path_denoiser_is_deterministic) {
  const PathTraceImage source = makeNoisyFlatImage();
  PathTraceImage first;
  PathTraceImage second;
  CHECK(denoisePathTraceImage(source, first));
  CHECK(denoisePathTraceImage(source, second));
  CHECK(sameImage(first, second));
}

TEST(path_denoiser_supports_in_place_output) {
  PathTraceImage source = makeNoisyFlatImage();
  const PathTraceImage original = source;
  CHECK(denoisePathTraceImage(source, source));
  CHECK(source.linearPixels != original.linearPixels);
  CHECK_EQ(source.samplesPerPixel, original.samplesPerPixel);
}

TEST(path_denoiser_rejects_malformed_and_nonfinite_inputs_without_publishing) {
  PathTraceImage output;
  output.width = 1;
  output.height = 1;
  output.samplesPerPixel = 77;
  output.linearPixels = {Color{0.2f, 0.3f, 0.4f, 0.5f}};
  const PathTraceImage originalOutput = output;

  PathTraceImage malformed;
  malformed.width = 2;
  malformed.height = 2;
  malformed.linearPixels.resize(3);
  CHECK(!denoisePathTraceImage(malformed, output));
  CHECK(sameImage(output, originalOutput));

  malformed.width = 0;
  malformed.height = 1;
  malformed.linearPixels.resize(1);
  CHECK(!denoisePathTraceImage(malformed, output));
  CHECK(sameImage(output, originalOutput));

  malformed.width = 1;
  malformed.height = 1;
  malformed.linearPixels[0].g = std::numeric_limits<float>::quiet_NaN();
  CHECK(!denoisePathTraceImage(malformed, output));
  CHECK(sameImage(output, originalOutput));
  malformed.linearPixels[0].g = 0.0f;
  malformed.linearPixels[0].a = std::numeric_limits<float>::infinity();
  CHECK(!denoisePathTraceImage(malformed, output));
  CHECK(sameImage(output, originalOutput));

  malformed.linearPixels[0].a = 1.0f;
  PathDenoiserSettings invalidSettings;
  invalidSettings.radius = 9;
  CHECK(!denoisePathTraceImage(malformed, output, invalidSettings));
  invalidSettings.radius = 2;
  invalidSettings.colorSigma = 0.0f;
  CHECK(!denoisePathTraceImage(malformed, output, invalidSettings));
  CHECK(sameImage(output, originalOutput));
}

HARNESS_MAIN();
