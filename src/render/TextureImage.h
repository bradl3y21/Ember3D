#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "core/Types.h"

namespace ember::render {

// CPU-side image data used by render backends. Rows are stored top-to-bottom;
// UVs use the usual image-texture convention where v=0 is the bottom edge.
struct TextureImage {
  int width = 0;
  int height = 0;
  std::vector<ember::Color> linearRgba;
};

// Loads dependency-free BMP (uncompressed 24/32-bit), PPM (P3/P6), and PFM
// (PF/Pf) images. Relative paths are resolved against baseDirectory (or the
// process working directory when it is empty). PPM and BMP RGB channels are
// decoded as sRGB only for ImageColorSpace::SRgb; the stored 32-bit BMP alpha
// byte is interpreted as linear opacity, and PFM is always scene-linear. BMP
// rows are normalized to top-to-bottom storage. Invalid, oversized, truncated,
// or non-finite images return std::nullopt.
std::optional<TextureImage> loadTextureImage(
    const std::filesystem::path& path,
    const std::filesystem::path& baseDirectory,
    ember::ImageColorSpace colorSpace);

// Bilinear, texel-center sampling in linear RGBA. repeat=true wraps both axes;
// otherwise UVs clamp to the image border. Non-finite UVs or invalid image
// storage return transparent black.
ember::Color sampleTextureImage(const TextureImage& texture, ember::Vec2 uv,
                                bool repeat = true);

} // namespace ember::render
