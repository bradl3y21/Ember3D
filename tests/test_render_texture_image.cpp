// CPU-only tests for the BMP/PPM/PFM texture loader and linear bilinear sampler.
#include "harness.h"

#include "render/TextureImage.h"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace ember;
using namespace ember::render;

namespace {

struct TempDirectory {
  std::filesystem::path path;
  TempDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("ember_texture_image_" + std::to_string(stamp));
    std::filesystem::create_directories(path);
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

bool writeBytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(file);
}

void appendLittleEndianFloat(std::string& bytes, float value) {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
  for (int shift = 0; shift < 32; shift += 8)
    bytes.push_back(static_cast<char>((bits >> shift) & 0xffu));
}

void appendBigEndianFloat(std::string& bytes, float value) {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
  for (int shift = 24; shift >= 0; shift -= 8)
    bytes.push_back(static_cast<char>((bits >> shift) & 0xffu));
}

void appendU16LE(std::string& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<char>(value & 0xffu));
  bytes.push_back(static_cast<char>((value >> 8u) & 0xffu));
}

void appendU32LE(std::string& bytes, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8)
    bytes.push_back(static_cast<char>((value >> shift) & 0xffu));
}

void setU16LE(std::string& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<char>(value & 0xffu);
  bytes[offset + 1] = static_cast<char>((value >> 8u) & 0xffu);
}

void setU32LE(std::string& bytes, std::size_t offset, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8)
    bytes[offset + static_cast<std::size_t>(shift / 8)] =
        static_cast<char>((value >> shift) & 0xffu);
}

std::string makeBmp(std::int32_t width, std::int32_t height, std::uint16_t bitsPerPixel,
                    const std::string& raster) {
  const std::uint32_t pixelOffset = 54;
  std::string bytes = "BM";
  appendU32LE(bytes, pixelOffset + static_cast<std::uint32_t>(raster.size()));
  appendU16LE(bytes, 0);
  appendU16LE(bytes, 0);
  appendU32LE(bytes, pixelOffset);
  appendU32LE(bytes, 40); // BITMAPINFOHEADER
  appendU32LE(bytes, std::bit_cast<std::uint32_t>(width));
  appendU32LE(bytes, std::bit_cast<std::uint32_t>(height));
  appendU16LE(bytes, 1);
  appendU16LE(bytes, bitsPerPixel);
  appendU32LE(bytes, 0); // BI_RGB
  appendU32LE(bytes, static_cast<std::uint32_t>(raster.size()));
  appendU32LE(bytes, 2835);
  appendU32LE(bytes, 2835);
  appendU32LE(bytes, 0);
  appendU32LE(bytes, 0);
  bytes += raster;
  return bytes;
}

float srgbToLinear(float value) {
  return value <= 0.04045f ? value / 12.92f
                           : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

} // namespace

TEST(ppm_ascii_srgb_and_linear_modes_decode_to_linear_rgba) {
  TempDirectory temp;
  const auto path = temp.path / "ascii.ppm";
  CHECK(writeBytes(path, "P3\n# dimensions follow\n2 1\n255\n0 0 0 255 128 64\n"));

  const auto srgb = loadTextureImage("ascii.ppm", temp.path, ImageColorSpace::SRgb);
  const auto linear = loadTextureImage(path, {}, ImageColorSpace::Linear);
  CHECK(srgb.has_value());
  CHECK(linear.has_value());
  if (!srgb || !linear) return;
  CHECK_EQ(srgb->width, 2);
  CHECK_EQ(srgb->height, 1);
  CHECK_EQ(srgb->linearRgba.size(), 2u);
  CHECK_NEAR(srgb->linearRgba[0].r, 0.0f, 1e-7f);
  CHECK_NEAR(srgb->linearRgba[1].r, 1.0f, 1e-7f);
  CHECK_NEAR(srgb->linearRgba[1].g, srgbToLinear(128.0f / 255.0f), 1e-6f);
  CHECK_NEAR(srgb->linearRgba[1].b, srgbToLinear(64.0f / 255.0f), 1e-6f);
  CHECK_NEAR(linear->linearRgba[1].g, 128.0f / 255.0f, 1e-6f);
  CHECK_EQ(srgb->linearRgba[1].a, 1.0f);
}

TEST(ppm_binary_supports_sixteen_bit_big_endian_samples) {
  TempDirectory temp;
  std::string bytes = "P6\n1 1\n1023\n";
  // P6's 16-bit samples are network-order (big-endian).
  bytes.append("\x02\x00\x03\xff\x00\x00", 6);
  CHECK(writeBytes(temp.path / "wide.ppm", bytes));
  const auto image = loadTextureImage("wide.ppm", temp.path, ImageColorSpace::Linear);
  CHECK(image.has_value());
  if (!image) return;
  CHECK_NEAR(image->linearRgba[0].r, 512.0f / 1023.0f, 1e-6f);
  CHECK_NEAR(image->linearRgba[0].g, 1.0f, 1e-6f);
  CHECK_NEAR(image->linearRgba[0].b, 0.0f, 1e-6f);
}

TEST(ppm_binary_raster_bytes_that_look_like_header_whitespace_are_data) {
  TempDirectory temp;
  std::string bytes = "P6\n1 1\n255\n";
  bytes.append(" \t#", 3);
  CHECK(writeBytes(temp.path / "markers.ppm", bytes));
  const auto image = loadTextureImage("markers.ppm", temp.path, ImageColorSpace::Linear);
  CHECK(image.has_value());
  if (!image) return;
  CHECK_NEAR(image->linearRgba[0].r, 32.0f / 255.0f, 1e-6f);
  CHECK_NEAR(image->linearRgba[0].g, 9.0f / 255.0f, 1e-6f);
  CHECK_NEAR(image->linearRgba[0].b, 35.0f / 255.0f, 1e-6f);
}

TEST(pfm_is_linear_applies_scale_and_flips_bottom_up_file_rows) {
  TempDirectory temp;
  std::string bytes = "PF\n1 2\n-2.0\n";
  // PFM stores the bottom row first; all channel values are multiplied by |scale|.
  for (float value : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f})
    appendLittleEndianFloat(bytes, value);
  CHECK(writeBytes(temp.path / "hdr.pfm", bytes));
  const auto image = loadTextureImage("hdr.pfm", temp.path, ImageColorSpace::SRgb);
  CHECK(image.has_value());
  if (!image) return;
  CHECK_EQ(image->width, 1);
  CHECK_EQ(image->height, 2);
  CHECK_NEAR(image->linearRgba[0].r, 8.0f, 1e-6f); // top row
  CHECK_NEAR(image->linearRgba[0].g, 10.0f, 1e-6f);
  CHECK_NEAR(image->linearRgba[0].b, 12.0f, 1e-6f);
  CHECK_NEAR(image->linearRgba[1].r, 2.0f, 1e-6f); // bottom row
  CHECK_NEAR(image->linearRgba[1].g, 4.0f, 1e-6f);
  CHECK_NEAR(image->linearRgba[1].b, 6.0f, 1e-6f);
}

TEST(pfm_grayscale_replicates_channels_and_is_not_srgb_decoded) {
  TempDirectory temp;
  std::string bytes = "Pf\n1 1\n-1.0\n";
  appendLittleEndianFloat(bytes, 0.25f);
  CHECK(writeBytes(temp.path / "gray.pfm", bytes));
  const auto image = loadTextureImage("gray.pfm", temp.path, ImageColorSpace::SRgb);
  CHECK(image.has_value());
  if (!image) return;
  CHECK_NEAR(image->linearRgba[0].r, 0.25f, 1e-7f);
  CHECK_NEAR(image->linearRgba[0].g, 0.25f, 1e-7f);
  CHECK_NEAR(image->linearRgba[0].b, 0.25f, 1e-7f);
}

TEST(pfm_supports_big_endian_file_samples) {
  TempDirectory temp;
  std::string bytes = "PF\n1 1\n1.0\n";
  appendBigEndianFloat(bytes, 1.0f);
  appendBigEndianFloat(bytes, 0.5f);
  appendBigEndianFloat(bytes, 0.25f);
  CHECK(writeBytes(temp.path / "big.pfm", bytes));
  const auto image = loadTextureImage("big.pfm", temp.path, ImageColorSpace::Linear);
  CHECK(image.has_value());
  if (!image) return;
  CHECK_NEAR(image->linearRgba[0].r, 1.0f, 1e-7f);
  CHECK_NEAR(image->linearRgba[0].g, 0.5f, 1e-7f);
  CHECK_NEAR(image->linearRgba[0].b, 0.25f, 1e-7f);
}

TEST(bmp_24_bit_bottom_up_rows_and_row_padding_decode_to_linear_rgba) {
  TempDirectory temp;
  std::string raster;
  // Three BGR pixels per row occupy 9 bytes; each row has 3 padding bytes.
  // BMP stores the bottom row first when height is positive.
  raster.append("\x00\xff\xff\xff\x00\xff\xff\xff\x00", 9); // yellow, magenta, cyan
  raster.append("\xA1\xA2\xA3", 3);
  raster.append("\x00\x00\xff\x00\xff\x00\xff\x00\x00", 9); // red, green, blue
  raster.append("\xB1\xB2\xB3", 3);
  CHECK(writeBytes(temp.path / "bottom-up.bmp", makeBmp(3, 2, 24, raster)));

  const auto image = loadTextureImage("bottom-up.bmp", temp.path, ImageColorSpace::Linear);
  CHECK(image.has_value());
  if (!image) return;
  CHECK_EQ(image->width, 3);
  CHECK_EQ(image->height, 2);
  CHECK_NEAR(image->linearRgba[0].r, 1.0f, 1e-7f); // top-left red
  CHECK_NEAR(image->linearRgba[0].g, 0.0f, 1e-7f);
  CHECK_NEAR(image->linearRgba[1].g, 1.0f, 1e-7f); // top middle green
  CHECK_NEAR(image->linearRgba[2].b, 1.0f, 1e-7f); // top-right blue
  CHECK_NEAR(image->linearRgba[3].r, 1.0f, 1e-7f); // bottom-left yellow
  CHECK_NEAR(image->linearRgba[3].g, 1.0f, 1e-7f);
  CHECK_NEAR(image->linearRgba[4].r, 1.0f, 1e-7f); // bottom middle magenta
  CHECK_NEAR(image->linearRgba[4].b, 1.0f, 1e-7f);
  CHECK_NEAR(image->linearRgba[5].g, 1.0f, 1e-7f); // bottom-right cyan
  CHECK_NEAR(image->linearRgba[5].b, 1.0f, 1e-7f);
  CHECK_EQ(image->linearRgba[0].a, 1.0f);
}

TEST(bmp_32_bit_top_down_preserves_alpha_and_honors_explicit_color_space) {
  TempDirectory temp;
  std::string raster;
  raster.append("\x40\x80\x80\x80", 4); // top-left BGRA
  raster.append("\x00\xff\xff\xff", 4);
  raster.append("\xff\x00\x00\x00", 4);
  raster.append("\x00\x00\x00\x40", 4);
  CHECK(writeBytes(temp.path / "top-down.bmp", makeBmp(2, -2, 32, raster)));

  const auto srgb = loadTextureImage("top-down.bmp", temp.path, ImageColorSpace::SRgb);
  const auto linear = loadTextureImage("top-down.bmp", temp.path, ImageColorSpace::Linear);
  CHECK(srgb.has_value());
  CHECK(linear.has_value());
  if (!srgb || !linear) return;
  CHECK_NEAR(srgb->linearRgba[0].r, srgbToLinear(128.0f / 255.0f), 1e-6f);
  CHECK_NEAR(linear->linearRgba[0].r, 128.0f / 255.0f, 1e-6f);
  CHECK_NEAR(linear->linearRgba[0].g, 128.0f / 255.0f, 1e-6f);
  CHECK_NEAR(linear->linearRgba[0].b, 64.0f / 255.0f, 1e-6f);
  CHECK_NEAR(linear->linearRgba[0].a, 128.0f / 255.0f, 1e-6f);
  CHECK_EQ(linear->linearRgba[2].a, 0.0f);
  CHECK_NEAR(linear->linearRgba[3].a, 64.0f / 255.0f, 1e-6f);
}

TEST(bmp_loader_rejects_malformed_headers_truncated_raster_and_unsupported_modes) {
  TempDirectory temp;
  const std::string valid = makeBmp(1, 1, 24, std::string("\x10\x20\x30\x00", 4));
  CHECK(writeBytes(temp.path / "truncated-header.bmp", valid.substr(0, 20)));
  CHECK(writeBytes(temp.path / "truncated-raster.bmp", valid.substr(0, valid.size() - 1)));

  std::string badSignature = valid;
  badSignature[0] = 'Z';
  CHECK(writeBytes(temp.path / "bad-signature.bmp", badSignature));
  std::string badOffset = valid;
  setU32LE(badOffset, 10, 53);
  CHECK(writeBytes(temp.path / "bad-offset.bmp", badOffset));
  std::string offsetPastEnd = valid;
  setU32LE(offsetPastEnd, 10, 4096);
  CHECK(writeBytes(temp.path / "offset-past-end.bmp", offsetPastEnd));
  std::string badPlanes = valid;
  setU16LE(badPlanes, 26, 2);
  CHECK(writeBytes(temp.path / "bad-planes.bmp", badPlanes));
  std::string unsupportedDepth = valid;
  setU16LE(unsupportedDepth, 28, 16);
  CHECK(writeBytes(temp.path / "unsupported-depth.bmp", unsupportedDepth));
  std::string compressed = valid;
  setU32LE(compressed, 30, 1);
  CHECK(writeBytes(temp.path / "compressed.bmp", compressed));
  std::string shortDeclaredSize = valid;
  setU32LE(shortDeclaredSize, 2, 57);
  CHECK(writeBytes(temp.path / "short-size.bmp", shortDeclaredSize));
  std::string oversizedDeclaredSize = valid;
  setU32LE(oversizedDeclaredSize, 2, 4096);
  CHECK(writeBytes(temp.path / "oversized-size.bmp", oversizedDeclaredSize));
  std::string invalidWidth = makeBmp(-1, 1, 24, std::string("\0\0\0\0", 4));
  CHECK(writeBytes(temp.path / "negative-width.bmp", invalidWidth));
  std::string invalidHeight = makeBmp(1, std::numeric_limits<std::int32_t>::min(), 24, {});
  CHECK(writeBytes(temp.path / "min-height.bmp", invalidHeight));
  std::string shortDib = valid;
  setU32LE(shortDib, 14, 39);
  CHECK(writeBytes(temp.path / "short-dib.bmp", shortDib));
  std::string oversizedDib = valid;
  setU32LE(oversizedDib, 14, 4096);
  CHECK(writeBytes(temp.path / "oversized-dib.bmp", oversizedDib));

  for (const char* name : {"truncated-header.bmp", "truncated-raster.bmp", "bad-signature.bmp",
                           "bad-offset.bmp", "offset-past-end.bmp",
                           "bad-planes.bmp", "unsupported-depth.bmp", "compressed.bmp",
                           "short-size.bmp", "oversized-size.bmp", "negative-width.bmp",
                           "min-height.bmp", "short-dib.bmp", "oversized-dib.bmp"})
    CHECK(!loadTextureImage(name, temp.path, ImageColorSpace::Linear));
}

TEST(bmp_loader_rejects_dimensions_over_pixel_and_dimension_limits_before_raster_read) {
  TempDirectory temp;
  CHECK(writeBytes(temp.path / "dimension-limit.bmp", makeBmp(40000, 1, 24, {})));
  CHECK(writeBytes(temp.path / "pixel-limit.bmp", makeBmp(5000, 4000, 24, {})));
  CHECK(!loadTextureImage("dimension-limit.bmp", temp.path, ImageColorSpace::Linear));
  CHECK(!loadTextureImage("pixel-limit.bmp", temp.path, ImageColorSpace::Linear));
}

TEST(loader_rejects_truncation_invalid_samples_nonfinite_hdr_and_bad_dimensions) {
  TempDirectory temp;
  CHECK(writeBytes(temp.path / "short.ppm", "P6\n1 1\n255\n\x01\x02"));
  CHECK(writeBytes(temp.path / "range.ppm", "P3\n1 1\n2\n0 3 0\n"));
  CHECK(writeBytes(temp.path / "huge.ppm", "P3\n40000 1\n255\n"));
  std::string nonfinite = "Pf\n1 1\n-1.0\n";
  appendLittleEndianFloat(nonfinite, std::numeric_limits<float>::quiet_NaN());
  CHECK(writeBytes(temp.path / "nan.pfm", nonfinite));
  std::string infinite = "Pf\n1 1\n-1.0\n";
  appendLittleEndianFloat(infinite, std::numeric_limits<float>::infinity());
  CHECK(writeBytes(temp.path / "inf.pfm", infinite));
  CHECK(writeBytes(temp.path / "inf-scale.pfm", "Pf\n1 1\ninf\n"));
  CHECK(!loadTextureImage("short.ppm", temp.path, ImageColorSpace::Linear));
  CHECK(!loadTextureImage("range.ppm", temp.path, ImageColorSpace::Linear));
  CHECK(!loadTextureImage("huge.ppm", temp.path, ImageColorSpace::Linear));
  CHECK(!loadTextureImage("nan.pfm", temp.path, ImageColorSpace::Linear));
  CHECK(!loadTextureImage("inf.pfm", temp.path, ImageColorSpace::Linear));
  CHECK(!loadTextureImage("inf-scale.pfm", temp.path, ImageColorSpace::Linear));
  CHECK(!loadTextureImage({}, temp.path, ImageColorSpace::SRgb));
  CHECK(!loadTextureImage("missing.ppm", temp.path, ImageColorSpace::SRgb));
}

TEST(sampler_is_bilinear_uses_bottom_left_uv_and_wraps_or_clamps) {
  TextureImage image;
  image.width = 2;
  image.height = 2;
  image.linearRgba = {
      Color{1, 0, 0, 1}, Color{0, 1, 0, 1}, // top row
      Color{0, 0, 1, 1}, Color{1, 1, 1, 1}, // bottom row
  };
  const Color topLeft = sampleTextureImage(image, {0.25f, 0.75f});
  CHECK_NEAR(topLeft.r, 1.0f, 1e-6f);
  CHECK_NEAR(topLeft.g, 0.0f, 1e-6f);
  const Color center = sampleTextureImage(image, {0.5f, 0.5f});
  CHECK_NEAR(center.r, 0.5f, 1e-6f);
  CHECK_NEAR(center.g, 0.5f, 1e-6f);
  CHECK_NEAR(center.b, 0.5f, 1e-6f);
  const Color wrapped = sampleTextureImage(image, {-0.75f, 0.75f});
  CHECK_NEAR(wrapped.r, topLeft.r, 1e-6f);
  CHECK_NEAR(wrapped.g, topLeft.g, 1e-6f);
  const Color clamped = sampleTextureImage(image, {1.25f, 0.75f}, false);
  CHECK_NEAR(clamped.r, 0.0f, 1e-6f);
  CHECK_NEAR(clamped.g, 1.0f, 1e-6f);
  CHECK_EQ(sampleTextureImage(image, {std::numeric_limits<float>::infinity(), 0}).a, 0.0f);
}

HARNESS_MAIN()
