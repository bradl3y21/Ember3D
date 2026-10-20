#pragma once

#include <filesystem>

#include "render/PathTracer.h"

namespace ember::render {

// Writes a scene-linear floating-point PFM image (RGB, no tone mapping or
// exposure). PFM has no alpha channel. The file uses little-endian samples,
// scale -1.0, and the format's required bottom-to-top row order. Values above
// 1.0 are preserved. Invalid dimensions, pixel counts, or non-finite RGBA
// samples fail without replacing an existing destination.
bool writePathImagePfm(const std::filesystem::path& path, const PathTraceImage& image);

// Writes a binary PPM (P6) preview. Linear RGB is clamped to [0,1], converted
// to sRGB, and quantized to 8 bits; alpha is omitted. This is a display-referred
// convenience format, not an HDR or lossless-linear output.
bool writePathImagePpm(const std::filesystem::path& path, const PathTraceImage& image);

} // namespace ember::render
