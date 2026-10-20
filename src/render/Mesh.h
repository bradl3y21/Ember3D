// Mesh.h — GPU buffers for one core::MeshData, plus an unlit line mesh for
// the M1 overlays (grid/axes).
//
// The renderer owns GPU lifetime (§2: "GPU resources are yours"). Upload
// happens on first sight of a MeshData pointer; see Renderer's cache for the
// staleness rules and limits.
#pragma once

#include <glad/gl.h>

#include <string>
#include <vector>

#include "core/Types.h"

namespace ember::render {

class Mesh {
public:
    Mesh() = default;
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    // Indexed triangle list from scene data. Interleaves position/normal/uv
    // (8 floats per vertex) into one VBO; UVs feed raster image textures.
    // Returns false (and leaves the mesh
    // empty) when the MeshData is malformed. An empty but consistent MeshData
    // is valid: it simply draws nothing.
    bool create(const MeshData& data);

    // Unlit line mesh for the viewport overlay: GL_LINES topology, position +
    // baked vertex colour (6 floats per vertex), no index buffer.
    bool createLines(const float* positions, const float* colors, std::size_t vertexCount);

    void destroy();
    bool valid() const { return vao_ != 0; }

    void drawElements() const;                                  // indexed triangles
    // Draw a contiguous range of indices from the indexed triangle list.
    // Renderer material batches use triangle-aligned ranges.
    void drawElements(GLsizei firstIndex, GLsizei indexCount) const;
    void drawLines(GLsizei count) const;                        // overlay helper
    GLsizei indexCount() const { return indexCount_; }
    GLsizei vertexCount() const { return vertexCount_; }
    const std::string& name() const { return name_; }

private:
    void clearHandles() noexcept;

    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    unsigned int ebo_ = 0;
    GLsizei vertexCount_ = 0;
    GLsizei indexCount_ = 0;
    std::string name_;
};

} // namespace ember::render
