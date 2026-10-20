#include "render/PathImageIO.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ember::render {
namespace {

static_assert(sizeof(float) == 4, "PFM support requires 32-bit float samples");
constexpr std::size_t kMaxPixels = 16u * 1024u * 1024u;
constexpr int kMaxDimension = 32768;
std::atomic<std::uint64_t> gTempCounter{0};
// Windows may transiently reject concurrent replacement of the same destination
// even though each rename is individually atomic. Keep encoding concurrent, but
// serialize the final publication step for writers in this process.
std::mutex gReplaceMutex;

bool validImage(const PathTraceImage& image) {
  if (image.width <= 0 || image.height <= 0 || image.width > kMaxDimension ||
      image.height > kMaxDimension) return false;
  const std::uint64_t pixelCount = static_cast<std::uint64_t>(image.width) *
                                   static_cast<std::uint64_t>(image.height);
  if (pixelCount > kMaxPixels || image.linearPixels.size() != pixelCount) return false;
  for (const ember::Color& pixel : image.linearPixels) {
    if (!std::isfinite(pixel.r) || !std::isfinite(pixel.g) ||
        !std::isfinite(pixel.b) || !std::isfinite(pixel.a)) return false;
  }
  return true;
}

std::filesystem::path temporarySibling(const std::filesystem::path& destination,
                                       std::uint64_t nonce) {
  std::filesystem::path temporary = destination;
  temporary += ".tmp." + std::to_string(nonce);
  return temporary;
}

// This stream buffer owns a descriptor opened with exclusive-create semantics.
// Writing through that descriptor (rather than reopening the path with ofstream)
// ensures no competing writer can truncate or substitute the reserved file.
class ExclusiveFileBuffer final : public std::streambuf {
public:
  explicit ExclusiveFileBuffer(int descriptor) : descriptor_(descriptor) {}
  ~ExclusiveFileBuffer() override { close(); }

  bool close() {
    if (descriptor_ < 0) return closeSucceeded_;
#ifdef _WIN32
    const int result = _close(descriptor_);
#else
    const int result = ::close(descriptor_);
#endif
    descriptor_ = -1;
    closeSucceeded_ = result == 0;
    return closeSucceeded_;
  }

protected:
  std::streamsize xsputn(const char* data, std::streamsize count) override {
    if (descriptor_ < 0 || count <= 0) return 0;
    std::streamsize written = 0;
    while (written < count) {
#ifdef _WIN32
      const unsigned int chunk = static_cast<unsigned int>(std::min<std::streamsize>(
          count - written, static_cast<std::streamsize>(std::numeric_limits<unsigned int>::max())));
      const int result = _write(descriptor_, data + written, chunk);
#else
      const std::size_t chunk = static_cast<std::size_t>(count - written);
      const ssize_t result = ::write(descriptor_, data + written, chunk);
#endif
      if (result < 0) {
#ifndef _WIN32
        if (errno == EINTR) continue;
#endif
        return written;
      }
      if (result == 0) return written;
      written += static_cast<std::streamsize>(result);
    }
    return written;
  }

  int_type overflow(int_type value) override {
    if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
    const char byte = traits_type::to_char_type(value);
    return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
  }

  int sync() override { return descriptor_ >= 0 ? 0 : -1; }

private:
  int descriptor_ = -1;
  bool closeSucceeded_ = true;
};

int createExclusive(const std::filesystem::path& path) {
#ifdef _WIN32
  return _wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                _S_IREAD | _S_IWRITE);
#else
  return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
}

bool replaceAtomically(const std::filesystem::path& temporary,
                       const std::filesystem::path& destination) {
#ifdef _WIN32
  return MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code error;
  std::filesystem::rename(temporary, destination, error);
  return !error;
#endif
}

template <typename Writer>
bool writeAtomically(const std::filesystem::path& destination, Writer&& writer) {
  if (destination.empty()) return false;
  std::filesystem::path temporary;
  bool ownsTemporary = false;
  try {
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t base = stamp ^ gTempCounter.fetch_add(1, std::memory_order_relaxed);
    int descriptor = -1;
    for (std::uint64_t attempt = 0; attempt < 32; ++attempt) {
      temporary = temporarySibling(destination, base + attempt);
      descriptor = createExclusive(temporary);
      if (descriptor >= 0) {
        ownsTemporary = true;
        break;
      }
    }
    if (descriptor < 0) return false;

    ExclusiveFileBuffer buffer(descriptor);
    std::ostream output(&buffer);
    const bool wrote = writer(output);
    output.flush();
    const bool flushed = static_cast<bool>(output);
    const bool closed = buffer.close();
    bool replaced = false;
    if (wrote && flushed && closed) {
      const std::lock_guard<std::mutex> lock(gReplaceMutex);
      replaced = replaceAtomically(temporary, destination);
    }
    if (!wrote || !flushed || !closed || !replaced) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      ownsTemporary = false;
      return false;
    }
    ownsTemporary = false;
    return true;
  } catch (...) {
    if (ownsTemporary) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
    }
    return false;
  }
}

void writeLittleEndianFloat(std::ostream& output, float value) {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
  const char bytes[4] = {
      static_cast<char>(bits & 0xffu),
      static_cast<char>((bits >> 8u) & 0xffu),
      static_cast<char>((bits >> 16u) & 0xffu),
      static_cast<char>((bits >> 24u) & 0xffu),
  };
  output.write(bytes, sizeof(bytes));
}

unsigned char toSrgbByte(float linear) {
  const float value = ember::clampf(linear, 0.0f, 1.0f);
  const float encoded = value <= 0.0031308f ? 12.92f * value
      : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
  const float rounded = std::floor(encoded * 255.0f + 0.5f);
  return static_cast<unsigned char>(ember::iclamp(static_cast<int>(rounded), 0, 255));
}

} // namespace

bool writePathImagePfm(const std::filesystem::path& path, const PathTraceImage& image) {
  if (!validImage(image)) return false;
  return writeAtomically(path, [&image](std::ostream& output) {
    output << "PF\n" << image.width << ' ' << image.height << "\n-1.0\n";
    for (int y = image.height - 1; y >= 0; --y) {
      for (int x = 0; x < image.width; ++x) {
        const ember::Color& pixel = image.linearPixels[
            static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
            static_cast<std::size_t>(x)];
        writeLittleEndianFloat(output, pixel.r);
        writeLittleEndianFloat(output, pixel.g);
        writeLittleEndianFloat(output, pixel.b);
      }
    }
    return static_cast<bool>(output);
  });
}

bool writePathImagePpm(const std::filesystem::path& path, const PathTraceImage& image) {
  if (!validImage(image)) return false;
  return writeAtomically(path, [&image](std::ostream& output) {
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (const ember::Color& pixel : image.linearPixels) {
      const unsigned char rgb[3] = {toSrgbByte(pixel.r), toSrgbByte(pixel.g),
                                    toSrgbByte(pixel.b)};
      output.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
    }
    return static_cast<bool>(output);
  });
}

} // namespace ember::render
