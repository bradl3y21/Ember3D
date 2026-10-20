#include "render/TextureImage.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>

namespace ember::render {
namespace {

static_assert(sizeof(float) == 4, "PFM support requires 32-bit float samples");
constexpr std::size_t kMaxPixels = 16u * 1024u * 1024u;
constexpr std::uintmax_t kMaxFileBytes = 512u * 1024u * 1024u;
constexpr int kMaxDimension = 32768;

bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

class HeaderReader {
 public:
  explicit HeaderReader(std::string_view bytes) : bytes_(bytes) {}

  bool token(std::string_view& out) {
    skipSpaceAndComments();
    if (position_ >= bytes_.size()) return false;
    const std::size_t start = position_;
    while (position_ < bytes_.size() && !isSpace(bytes_[position_]) && bytes_[position_] != '#')
      ++position_;
    if (position_ == start) return false;
    out = bytes_.substr(start, position_ - start);
    return true;
  }

  // Binary PPM/PFM have one whitespace separator between their final header
  // token and raster. Consume only that separator so raster bytes that happen
  // to look like whitespace or a comment marker remain data.
  bool binaryRaster(std::size_t& offset) {
    if (position_ >= bytes_.size() || !isSpace(bytes_[position_])) return false;
    const char first = bytes_[position_++];
    if (first == '\r' && position_ < bytes_.size() && bytes_[position_] == '\n') ++position_;
    offset = position_;
    return true;
  }

  std::size_t position() const { return position_; }

 private:
  void skipSpaceAndComments() {
    for (;;) {
      while (position_ < bytes_.size() && isSpace(bytes_[position_])) ++position_;
      if (position_ >= bytes_.size() || bytes_[position_] != '#') return;
      while (position_ < bytes_.size() && bytes_[position_] != '\n' && bytes_[position_] != '\r')
        ++position_;
    }
  }

  std::string_view bytes_;
  std::size_t position_ = 0;
};

bool parseUnsigned(std::string_view text, std::uint32_t& value) {
  if (text.empty()) return false;
  std::uint64_t parsed = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    parsed = parsed * 10u + static_cast<unsigned>(c - '0');
    if (parsed > std::numeric_limits<std::uint32_t>::max()) return false;
  }
  value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool parseFloat(std::string_view text, float& value) {
  if (text.empty() || text.size() >= 128) return false;
  char buffer[128];
  std::copy(text.begin(), text.end(), buffer);
  buffer[text.size()] = '\0';
  char* end = nullptr;
  const float parsed = std::strtof(buffer, &end);
  if (end != buffer + text.size() || !std::isfinite(parsed)) return false;
  value = parsed;
  return true;
}

bool dimensions(std::uint32_t width, std::uint32_t height, std::size_t& pixels) {
  if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) return false;
  const std::uint64_t product = static_cast<std::uint64_t>(width) * height;
  if (product > kMaxPixels || product > std::numeric_limits<std::size_t>::max()) return false;
  pixels = static_cast<std::size_t>(product);
  return true;
}

float srgbToLinear(float value) {
  return value <= 0.04045f ? value / 12.92f
                           : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

bool validColorSpace(ember::ImageColorSpace colorSpace) {
  return colorSpace == ember::ImageColorSpace::SRgb ||
         colorSpace == ember::ImageColorSpace::Linear;
}

std::uint16_t readU16LE(const unsigned char* bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8u);
}

std::uint32_t readU32LE(const unsigned char* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8u) |
         (static_cast<std::uint32_t>(bytes[2]) << 16u) |
         (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

bool parseBmp(std::string_view bytes, TextureImage& result,
              ember::ImageColorSpace colorSpace) {
  constexpr std::size_t kFileHeaderBytes = 14;
  constexpr std::size_t kInfoHeaderBytes = 40;
  if (bytes.size() < kFileHeaderBytes + kInfoHeaderBytes ||
      bytes[0] != 'B' || bytes[1] != 'M') return false;

  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  const std::uint32_t declaredFileSize = readU32LE(data + 2);
  const std::uint32_t pixelOffset = readU32LE(data + 10);
  const std::uint32_t dibSize = readU32LE(data + kFileHeaderBytes);
  if (dibSize < kInfoHeaderBytes || dibSize > bytes.size() - kFileHeaderBytes)
    return false;
  const std::uint64_t minimumPixelOffset =
      static_cast<std::uint64_t>(kFileHeaderBytes) + dibSize;
  if (pixelOffset < minimumPixelOffset || pixelOffset > bytes.size()) return false;

  const std::int32_t signedWidth =
      std::bit_cast<std::int32_t>(readU32LE(data + kFileHeaderBytes + 4));
  const std::int32_t signedHeight =
      std::bit_cast<std::int32_t>(readU32LE(data + kFileHeaderBytes + 8));
  const std::uint16_t planes = readU16LE(data + kFileHeaderBytes + 12);
  const std::uint16_t bitsPerPixel = readU16LE(data + kFileHeaderBytes + 14);
  const std::uint32_t compression = readU32LE(data + kFileHeaderBytes + 16);
  if (signedWidth <= 0 || signedHeight == 0 ||
      signedHeight == std::numeric_limits<std::int32_t>::min() || planes != 1 ||
      (bitsPerPixel != 24 && bitsPerPixel != 32) || compression != 0)
    return false;

  const std::uint32_t width = static_cast<std::uint32_t>(signedWidth);
  const std::uint32_t height = static_cast<std::uint32_t>(
      signedHeight < 0 ? -signedHeight : signedHeight);
  std::size_t pixelCount = 0;
  if (!dimensions(width, height, pixelCount)) return false;

  const std::uint64_t rowBits = static_cast<std::uint64_t>(width) * bitsPerPixel;
  const std::uint64_t rowStride64 = ((rowBits + 31u) / 32u) * 4u;
  const std::uint64_t rasterBytes64 = rowStride64 * height;
  if (rowStride64 > std::numeric_limits<std::size_t>::max() ||
      rasterBytes64 > std::numeric_limits<std::size_t>::max()) return false;
  const std::size_t rasterBytes = static_cast<std::size_t>(rasterBytes64);
  const std::size_t rasterOffset = static_cast<std::size_t>(pixelOffset);
  if (rasterOffset > bytes.size() || rasterBytes > bytes.size() - rasterOffset)
    return false;
  const std::size_t rasterEnd = rasterOffset + rasterBytes;
  if (declaredFileSize < rasterEnd || declaredFileSize > bytes.size()) return false;

  result.width = static_cast<int>(width);
  result.height = static_cast<int>(height);
  result.linearRgba.resize(pixelCount);
  const std::size_t rowStride = static_cast<std::size_t>(rowStride64);
  const std::size_t bytesPerPixel = bitsPerPixel / 8u;
  const auto toLinear = [colorSpace](unsigned char encoded) {
    const float value = static_cast<float>(encoded) / 255.0f;
    return colorSpace == ember::ImageColorSpace::SRgb ? srgbToLinear(value) : value;
  };
  for (std::uint32_t fileY = 0; fileY < height; ++fileY) {
    const std::uint32_t imageY = signedHeight > 0 ? height - 1u - fileY : fileY;
    const unsigned char* row = data + rasterOffset +
                                static_cast<std::size_t>(fileY) * rowStride;
    for (std::uint32_t x = 0; x < width; ++x) {
      const unsigned char* pixel = row + static_cast<std::size_t>(x) * bytesPerPixel;
      const float alpha = bitsPerPixel == 32
          ? static_cast<float>(pixel[3]) / 255.0f : 1.0f;
      result.linearRgba[static_cast<std::size_t>(imageY) * width + x] = {
          toLinear(pixel[2]), toLinear(pixel[1]), toLinear(pixel[0]), alpha};
    }
  }
  return true;
}

bool parsePpm(std::string_view bytes, HeaderReader& reader,
              ember::ImageColorSpace colorSpace, TextureImage& result,
              std::string_view magic) {
  std::string_view token;
  std::uint32_t width = 0, height = 0, maximum = 0;
  std::size_t pixelCount = 0;
  if (!reader.token(token) || !parseUnsigned(token, width) ||
      !reader.token(token) || !parseUnsigned(token, height) ||
      !reader.token(token) || !parseUnsigned(token, maximum) || maximum == 0 || maximum > 65535 ||
      !dimensions(width, height, pixelCount)) return false;

  const std::size_t channels = 3;
  const std::size_t samples = pixelCount * channels;
  result.width = static_cast<int>(width);
  result.height = static_cast<int>(height);
  const auto toLinear = [colorSpace, maximum](std::uint32_t sample) {
    const float encoded = static_cast<float>(sample) / static_cast<float>(maximum);
    return colorSpace == ember::ImageColorSpace::SRgb ? srgbToLinear(encoded) : encoded;
  };

  if (magic == "P3") {
    if (reader.position() > bytes.size() || bytes.size() - reader.position() < samples)
      return false;
    result.linearRgba.resize(pixelCount);
    for (std::size_t i = 0; i < pixelCount; ++i) {
      std::uint32_t rgb[3]{};
      for (std::uint32_t& component : rgb) {
        if (!reader.token(token) || !parseUnsigned(token, component) || component > maximum)
          return false;
      }
      result.linearRgba[i] = {toLinear(rgb[0]), toLinear(rgb[1]), toLinear(rgb[2]), 1.0f};
    }
    // P3 is textual: reject trailing non-comment tokens rather than silently
    // accepting a malformed or concatenated image.
    if (reader.token(token)) return false;
    return true;
  }

  std::size_t rasterOffset = 0;
  if (!reader.binaryRaster(rasterOffset)) return false;
  const std::size_t bytesPerSample = maximum < 256 ? 1u : 2u;
  if (samples > (std::numeric_limits<std::size_t>::max() / bytesPerSample)) return false;
  const std::size_t rasterBytes = samples * bytesPerSample;
  if (rasterOffset > bytes.size() || bytes.size() - rasterOffset != rasterBytes) return false;
  result.linearRgba.resize(pixelCount);
  const auto* raster = reinterpret_cast<const unsigned char*>(bytes.data() + rasterOffset);
  for (std::size_t i = 0; i < pixelCount; ++i) {
    std::uint32_t rgb[3]{};
    for (int c = 0; c < 3; ++c) {
      const std::size_t sampleIndex = i * 3u + static_cast<std::size_t>(c);
      rgb[c] = bytesPerSample == 1
                   ? raster[sampleIndex]
                   : (static_cast<std::uint32_t>(raster[sampleIndex * 2u]) << 8u) |
                         raster[sampleIndex * 2u + 1u];
      if (rgb[c] > maximum) return false;
    }
    result.linearRgba[i] = {toLinear(rgb[0]), toLinear(rgb[1]), toLinear(rgb[2]), 1.0f};
  }
  return true;
}

bool parsePfm(std::string_view bytes, HeaderReader& reader, TextureImage& result,
              std::string_view magic) {
  std::string_view token;
  std::uint32_t width = 0, height = 0;
  float scale = 0.0f;
  std::size_t pixelCount = 0;
  if (!reader.token(token) || !parseUnsigned(token, width) ||
      !reader.token(token) || !parseUnsigned(token, height) ||
      !reader.token(token) || !parseFloat(token, scale) || scale == 0.0f ||
      !dimensions(width, height, pixelCount)) return false;

  std::size_t rasterOffset = 0;
  if (!reader.binaryRaster(rasterOffset)) return false;
  const std::size_t channels = magic == "PF" ? 3u : 1u;
  const std::size_t sampleCount = pixelCount * channels;
  if (sampleCount > (std::numeric_limits<std::size_t>::max() / sizeof(float))) return false;
  const std::size_t rasterBytes = sampleCount * sizeof(float);
  if (rasterOffset > bytes.size() || bytes.size() - rasterOffset != rasterBytes) return false;

  const bool fileLittleEndian = scale < 0.0f;
  const float multiplier = std::fabs(scale);
  result.width = static_cast<int>(width);
  result.height = static_cast<int>(height);
  result.linearRgba.resize(pixelCount);
  const auto* raster = reinterpret_cast<const unsigned char*>(bytes.data() + rasterOffset);
  for (std::uint32_t fileY = 0; fileY < height; ++fileY) {
    const std::uint32_t imageY = height - 1u - fileY;
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t pixelIndex = static_cast<std::size_t>(imageY) * width + x;
      float values[3]{};
      for (std::size_t c = 0; c < channels; ++c) {
        const std::size_t sampleIndex =
            (static_cast<std::size_t>(fileY) * width + x) * channels + c;
        const unsigned char* source = raster + sampleIndex * 4u;
        const std::uint32_t bits = fileLittleEndian
            ? static_cast<std::uint32_t>(source[0]) |
                  (static_cast<std::uint32_t>(source[1]) << 8u) |
                  (static_cast<std::uint32_t>(source[2]) << 16u) |
                  (static_cast<std::uint32_t>(source[3]) << 24u)
            : (static_cast<std::uint32_t>(source[0]) << 24u) |
                  (static_cast<std::uint32_t>(source[1]) << 16u) |
                  (static_cast<std::uint32_t>(source[2]) << 8u) |
                  static_cast<std::uint32_t>(source[3]);
        const float sample = std::bit_cast<float>(bits) * multiplier;
        if (!std::isfinite(sample)) return false;
        values[c] = sample;
      }
      if (channels == 1) values[1] = values[2] = values[0];
      result.linearRgba[pixelIndex] = {values[0], values[1], values[2], 1.0f};
    }
  }
  return true;
}

int wrapIndex(int index, int size) {
  const int remainder = index % size;
  return remainder < 0 ? remainder + size : remainder;
}

ember::Color mix(ember::Color a, ember::Color b, float t) {
  return {ember::lerpf(a.r, b.r, t), ember::lerpf(a.g, b.g, t),
          ember::lerpf(a.b, b.b, t), ember::lerpf(a.a, b.a, t)};
}

} // namespace

std::optional<TextureImage> loadTextureImage(const std::filesystem::path& path,
                                             const std::filesystem::path& baseDirectory,
                                             ember::ImageColorSpace colorSpace) {
  if (path.empty() || !validColorSpace(colorSpace)) return std::nullopt;
  try {
    std::filesystem::path resolved = path;
    if (resolved.is_relative())
      resolved = (baseDirectory.empty() ? std::filesystem::current_path() : baseDirectory) / resolved;
    const std::uintmax_t fileBytes = std::filesystem::file_size(resolved);
    if (fileBytes < 3 || fileBytes > kMaxFileBytes ||
        fileBytes > std::numeric_limits<std::size_t>::max()) return std::nullopt;
    std::ifstream file(resolved, std::ios::binary);
    if (!file) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(fileBytes), '\0');
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;

    TextureImage image;
    bool ok = false;
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') {
      ok = parseBmp(bytes, image, colorSpace);
    } else {
      HeaderReader reader(bytes);
      std::string_view magic;
      if (!reader.token(magic)) return std::nullopt;
      if (magic == "P3" || magic == "P6")
        ok = parsePpm(bytes, reader, colorSpace, image, magic);
      else if (magic == "PF" || magic == "Pf")
        ok = parsePfm(bytes, reader, image, magic);
    }
    if (!ok || image.width <= 0 || image.height <= 0 ||
        image.linearRgba.size() != static_cast<std::size_t>(image.width) * image.height)
      return std::nullopt;
    for (const ember::Color& color : image.linearRgba) {
      if (!std::isfinite(color.r) || !std::isfinite(color.g) ||
          !std::isfinite(color.b) || !std::isfinite(color.a)) return std::nullopt;
    }
    return image;
  } catch (...) {
    return std::nullopt;
  }
}

ember::Color sampleTextureImage(const TextureImage& texture, ember::Vec2 uv, bool repeat) {
  const std::size_t expected = texture.width > 0 && texture.height > 0
      ? static_cast<std::size_t>(texture.width) * static_cast<std::size_t>(texture.height)
      : 0;
  if (expected == 0 || expected > kMaxPixels || texture.linearRgba.size() != expected ||
      !std::isfinite(uv.x) || !std::isfinite(uv.y)) return {0, 0, 0, 0};

  float u = uv.x;
  float v = uv.y;
  if (repeat) {
    u = std::fmod(u, 1.0f);
    v = std::fmod(v, 1.0f);
    if (u < 0.0f) u += 1.0f;
    if (v < 0.0f) v += 1.0f;
  } else {
    u = ember::clampf(u, 0.0f, 1.0f);
    v = ember::clampf(v, 0.0f, 1.0f);
  }

  const float x = u * static_cast<float>(texture.width) - 0.5f;
  const float y = (1.0f - v) * static_cast<float>(texture.height) - 0.5f;
  const int x0 = static_cast<int>(std::floor(x));
  const int y0 = static_cast<int>(std::floor(y));
  const float tx = x - static_cast<float>(x0);
  const float ty = y - static_cast<float>(y0);
  const auto index = [repeat](int coordinate, int size) {
    return repeat ? wrapIndex(coordinate, size) : ember::iclamp(coordinate, 0, size - 1);
  };
  const auto at = [&texture, &index](int ix, int iy) -> ember::Color {
    const int xIndex = index(ix, texture.width);
    const int yIndex = index(iy, texture.height);
    return texture.linearRgba[static_cast<std::size_t>(yIndex) * texture.width + xIndex];
  };
  return mix(mix(at(x0, y0), at(x0 + 1, y0), tx),
             mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx), ty);
}

} // namespace ember::render
