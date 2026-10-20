// CPU-only tests for documented final-image PFM and 8-bit PPM output.
#include "harness.h"

#include "render/PathImageIO.h"

#include <atomic>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace ember;
using namespace ember::render;

namespace {

struct TempDirectory {
  std::filesystem::path path;
  TempDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("ember_path_image_io_" + std::to_string(stamp));
    std::filesystem::create_directories(path);
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::string readBytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

float readLittleEndianFloat(const std::string& bytes, std::size_t offset) {
  const auto byte = [&bytes](std::size_t index) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index]));
  };
  const std::uint32_t bits = byte(offset) | (byte(offset + 1) << 8u) |
                             (byte(offset + 2) << 16u) | (byte(offset + 3) << 24u);
  return std::bit_cast<float>(bits);
}

PathTraceImage fixture() {
  PathTraceImage image;
  image.width = 2;
  image.height = 2;
  image.samplesPerPixel = 4;
  image.linearPixels = {
      Color{2.0f, 0.5f, 0.25f, 1.0f}, Color{0.0f, 1.0f, 4.0f, 0.5f}, // top
      Color{0.1f, 0.2f, 0.3f, 1.0f}, Color{8.0f, 16.0f, 32.0f, 1.0f}, // bottom
  };
  return image;
}

PathTraceImage solidImage(float value) {
  PathTraceImage image;
  image.width = 4;
  image.height = 3;
  image.samplesPerPixel = 1;
  image.linearPixels.assign(static_cast<std::size_t>(image.width * image.height),
                            Color{value, value + 0.125f, value + 0.25f, 1.0f});
  return image;
}

std::size_t temporaryFileCount(const std::filesystem::path& directory) {
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().find(".tmp.") != std::string::npos) ++count;
  }
  return count;
}

} // namespace

TEST(pfm_writes_linear_hdr_rgb_little_endian_and_bottom_up_rows) {
  TempDirectory temp;
  const auto path = temp.path / "final.pfm";
  CHECK(writePathImagePfm(path, fixture()));
  const std::string bytes = readBytes(path);
  const std::string header = "PF\n2 2\n-1.0\n";
  CHECK(bytes.size() == header.size() + 2u * 2u * 3u * sizeof(float));
  CHECK(bytes.compare(0, header.size(), header) == 0);
  if (bytes.size() < header.size() + 48u) return;
  const std::size_t data = header.size();
  // First file row is the image's bottom row; alpha is not stored.
  CHECK_NEAR(readLittleEndianFloat(bytes, data + 0), 0.1f, 1e-7f);
  CHECK_NEAR(readLittleEndianFloat(bytes, data + 4), 0.2f, 1e-7f);
  CHECK_NEAR(readLittleEndianFloat(bytes, data + 8), 0.3f, 1e-7f);
  CHECK_NEAR(readLittleEndianFloat(bytes, data + 12), 8.0f, 1e-7f);
  CHECK_NEAR(readLittleEndianFloat(bytes, data + 24), 2.0f, 1e-7f);
  CHECK_NEAR(readLittleEndianFloat(bytes, data + 28), 0.5f, 1e-7f);
  CHECK_NEAR(readLittleEndianFloat(bytes, data + 32), 0.25f, 1e-7f);
}

TEST(ppm_writes_top_down_srgb_bytes_with_clamping) {
  TempDirectory temp;
  const auto path = temp.path / "preview.ppm";
  CHECK(writePathImagePpm(path, fixture()));
  const std::string bytes = readBytes(path);
  const std::string header = "P6\n2 2\n255\n";
  CHECK(bytes.size() == header.size() + 12u);
  CHECK(bytes.compare(0, header.size(), header) == 0);
  if (bytes.size() < header.size() + 12u) return;
  const std::size_t pixels = header.size();
  CHECK_EQ(static_cast<unsigned char>(bytes[pixels + 0]), 255u); // linear 2 clamps
  CHECK_EQ(static_cast<unsigned char>(bytes[pixels + 1]), 188u); // linear .5 -> sRGB
  CHECK_EQ(static_cast<unsigned char>(bytes[pixels + 2]), 137u); // linear .25 -> sRGB
  CHECK_EQ(static_cast<unsigned char>(bytes[pixels + 3]), 0u);
  CHECK_EQ(static_cast<unsigned char>(bytes[pixels + 4]), 255u);
  CHECK_EQ(static_cast<unsigned char>(bytes[pixels + 5]), 255u); // linear 4 clamps
  // The second file row is the source's bottom row, not a vertical flip.
  CHECK_EQ(static_cast<unsigned char>(bytes[pixels + 6]), 89u); // linear .1 -> sRGB
  CHECK_EQ(static_cast<unsigned char>(bytes[pixels + 7]), 124u); // linear .2 -> sRGB
  CHECK_EQ(static_cast<unsigned char>(bytes[pixels + 8]), 149u); // linear .3 -> sRGB
  CHECK_EQ(static_cast<unsigned char>(bytes[pixels + 9]), 255u); // linear 8 clamps
}

TEST(pfm_preserves_hdr_values_above_one_without_tone_mapping) {
  TempDirectory temp;
  const auto path = temp.path / "hdr.pfm";
  PathTraceImage image = solidImage(32.0f);
  CHECK(writePathImagePfm(path, image));
  const std::string bytes = readBytes(path);
  const std::size_t data = std::string("PF\n4 3\n-1.0\n").size();
  CHECK_EQ(bytes.size(), data + 4u * 3u * 3u * sizeof(float));
  if (bytes.size() < data + 12u) return;
  CHECK_NEAR(readLittleEndianFloat(bytes, data + 0), 32.0f, 0.0f);
  CHECK_NEAR(readLittleEndianFloat(bytes, data + 4), 32.125f, 0.0f);
  CHECK_NEAR(readLittleEndianFloat(bytes, data + 8), 32.25f, 0.0f);
  CHECK_NEAR(readLittleEndianFloat(bytes, data + 12), 32.0f, 0.0f);
}

TEST(invalid_nonfinite_or_mismatched_images_fail_without_replacing_destination) {
  TempDirectory temp;
  const auto path = temp.path / "existing.pfm";
  {
    std::ofstream file(path, std::ios::binary);
    file << "keep-this-file";
  }
  const std::string original = readBytes(path);

  PathTraceImage invalid = fixture();
  invalid.linearPixels[1].g = std::numeric_limits<float>::infinity();
  CHECK(!writePathImagePfm(path, invalid));
  CHECK(!writePathImagePpm(path, invalid));
  CHECK(readBytes(path) == original);

  invalid = fixture();
  invalid.linearPixels.pop_back();
  CHECK(!writePathImagePfm(path, invalid));
  CHECK(!writePathImagePpm(path, invalid));
  CHECK(readBytes(path) == original);
  CHECK(!writePathImagePfm({}, fixture()));
}

TEST(concurrent_pfm_writers_never_publish_mixed_or_partial_files) {
  TempDirectory temp;
  const auto path = temp.path / "contended.pfm";
  constexpr int threadCount = 8;
  constexpr int writesPerThread = 12;
  std::atomic<bool> allWritesSucceeded{true};
  std::atomic<int> failedWrites{0};
  std::vector<float> values;
  std::vector<std::thread> writers;
  for (int thread = 0; thread < threadCount; ++thread) {
    const float value = 2.0f + static_cast<float>(thread);
    values.push_back(value);
    writers.emplace_back([&, value] {
      const PathTraceImage image = solidImage(value);
      for (int write = 0; write < writesPerThread; ++write) {
        if (!writePathImagePfm(path, image)) {
          allWritesSucceeded.store(false);
          failedWrites.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& writer : writers) writer.join();
  CHECK(allWritesSucceeded.load());
  CHECK_EQ(failedWrites.load(), 0);

  const std::string bytes = readBytes(path);
  const std::size_t data = std::string("PF\n4 3\n-1.0\n").size();
  CHECK_EQ(bytes.size(), data + 4u * 3u * 3u * sizeof(float));
  if (bytes.size() < data + 4u * 3u * 3u * sizeof(float)) return;
  const float first = readLittleEndianFloat(bytes, data);
  bool belongsToOneWriter = false;
  for (float value : values) belongsToOneWriter |= first == value;
  CHECK(belongsToOneWriter);
  for (std::size_t offset = data; offset < bytes.size(); offset += 12u) {
    CHECK_NEAR(readLittleEndianFloat(bytes, offset), first, 0.0f);
    CHECK_NEAR(readLittleEndianFloat(bytes, offset + 4), first + 0.125f, 0.0f);
    CHECK_NEAR(readLittleEndianFloat(bytes, offset + 8), first + 0.25f, 0.0f);
  }
  CHECK_EQ(temporaryFileCount(temp.path), 0u);
}

TEST(concurrent_ppm_writers_never_publish_mixed_or_partial_files) {
  TempDirectory temp;
  const auto path = temp.path / "contended.ppm";
  constexpr int threadCount = 8;
  constexpr int writesPerThread = 12;
  std::atomic<bool> allWritesSucceeded{true};
  std::atomic<int> failedWrites{0};
  std::vector<std::array<unsigned char, 3>> colors;
  std::vector<std::thread> writers;
  for (int thread = 0; thread < threadCount; ++thread) {
    const float value = static_cast<float>(thread + 1) * 0.05f;
    const PathTraceImage image = solidImage(value);
    const auto encoded = std::array<unsigned char, 3>{
        static_cast<unsigned char>(std::floor((1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f) * 255.0f + 0.5f)),
        static_cast<unsigned char>(std::floor((1.055f * std::pow(value + 0.125f, 1.0f / 2.4f) - 0.055f) * 255.0f + 0.5f)),
        static_cast<unsigned char>(std::floor((1.055f * std::pow(value + 0.25f, 1.0f / 2.4f) - 0.055f) * 255.0f + 0.5f)),
    };
    colors.push_back(encoded);
    writers.emplace_back([&, image] {
      for (int write = 0; write < writesPerThread; ++write) {
        if (!writePathImagePpm(path, image)) {
          allWritesSucceeded.store(false);
          failedWrites.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& writer : writers) writer.join();
  CHECK(allWritesSucceeded.load());
  CHECK_EQ(failedWrites.load(), 0);

  const std::string bytes = readBytes(path);
  const std::size_t data = std::string("P6\n4 3\n255\n").size();
  CHECK_EQ(bytes.size(), data + 4u * 3u * 3u);
  if (bytes.size() < data + 4u * 3u * 3u) return;
  const std::array<unsigned char, 3> first{
      static_cast<unsigned char>(bytes[data]), static_cast<unsigned char>(bytes[data + 1]),
      static_cast<unsigned char>(bytes[data + 2])};
  bool belongsToOneWriter = false;
  for (const auto& color : colors) belongsToOneWriter |= first == color;
  CHECK(belongsToOneWriter);
  for (std::size_t offset = data; offset < bytes.size(); offset += 3u) {
    CHECK_EQ(static_cast<unsigned char>(bytes[offset]), first[0]);
    CHECK_EQ(static_cast<unsigned char>(bytes[offset + 1]), first[1]);
    CHECK_EQ(static_cast<unsigned char>(bytes[offset + 2]), first[2]);
  }
  CHECK_EQ(temporaryFileCount(temp.path), 0u);
}

TEST(replacement_failure_keeps_existing_destination_directory_intact) {
  TempDirectory temp;
  const auto destination = temp.path / "occupied";
  std::filesystem::create_directory(destination);
  const auto sentinel = destination / "sentinel.txt";
  {
    std::ofstream file(sentinel, std::ios::binary);
    file << "keep-existing-data";
  }
  const std::string original = readBytes(sentinel);

  CHECK(!writePathImagePfm(destination, fixture()));
  CHECK(!writePathImagePpm(destination, fixture()));
  CHECK(std::filesystem::is_directory(destination));
  CHECK(readBytes(sentinel) == original);
  CHECK_EQ(temporaryFileCount(temp.path), 0u);
}

HARNESS_MAIN()
