// Mesh.cpp — VAO/VBO/EBO upload from core::MeshData.
#include "render/Mesh.h"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ember::render {

namespace {

// Scene meshes: position(3) + normal(3) + uv(2).
constexpr GLsizei kSceneStrideFloats = 8;
// Overlay lines: position(3) + color(3).
constexpr GLsizei kLineStrideFloats = 6;

} // namespace

Mesh::~Mesh() {
    destroy();
}

Mesh::Mesh(Mesh&& other) noexcept
    : vao_(other.vao_), vbo_(other.vbo_), ebo_(other.ebo_),
      vertexCount_(other.vertexCount_), indexCount_(other.indexCount_),
      name_(std::move(other.name_)) {
    other.clearHandles();
    other.vertexCount_ = 0;
    other.indexCount_ = 0;
}

Mesh& Mesh::operator=(Mesh&& other) noexcept {
    if (this != &other) {
        destroy();
        vao_ = other.vao_;
        vbo_ = other.vbo_;
        ebo_ = other.ebo_;
        vertexCount_ = other.vertexCount_;
        indexCount_ = other.indexCount_;
        name_ = std::move(other.name_);
        other.clearHandles();
        other.vertexCount_ = 0;
        other.indexCount_ = 0;
    }
    return *this;
}

void Mesh::clearHandles() noexcept {
    vao_ = vbo_ = ebo_ = 0;
}

bool Mesh::create(const MeshData& data) {
    destroy();

    const std::size_t vertices = data.positions.size() / 3;
    if (vertices == 0) {
        // Consistent-but-empty is valid: it simply draws nothing.
        name_ = data.name;
        return data.normals.empty() && data.uvs.empty() && data.indices.empty();
    }
    if (data.normals.size() != vertices * 3 || data.uvs.size() != vertices * 2) {
        return false;
    }
    // Non-finite UVs make GPU texture lookup undefined; reject malformed
    // vertex attributes before uploading them to GPU buffers.
    for (const float value : data.positions) if (!std::isfinite(value)) return false;
    for (const float value : data.normals) if (!std::isfinite(value)) return false;
    for (const float value : data.uvs) if (!std::isfinite(value)) return false;
    if (!data.indices.empty()) {
        u32 maxIndex = 0;
        for (const u32 i : data.indices) {
            if (i > maxIndex) maxIndex = i;
        }
        if (static_cast<std::size_t>(maxIndex) >= vertices) {
            return false;
        }
    }

    std::vector<float> interleaved(vertices * kSceneStrideFloats);
    for (std::size_t v = 0; v < vertices; ++v) {
        float* out = &interleaved[v * kSceneStrideFloats];
        out[0] = data.positions[v * 3 + 0];
        out[1] = data.positions[v * 3 + 1];
        out[2] = data.positions[v * 3 + 2];
        out[3] = data.normals[v * 3 + 0];
        out[4] = data.normals[v * 3 + 1];
        out[5] = data.normals[v * 3 + 2];
        out[6] = data.uvs[v * 2 + 0];
        out[7] = data.uvs[v * 2 + 1];
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(interleaved.size() * sizeof(float)),
                 interleaved.data(), GL_STATIC_DRAW);
    const GLsizei stride = kSceneStrideFloats * static_cast<GLsizei>(sizeof(float));
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(3 * sizeof(float)));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    if (!data.indices.empty()) {
        glGenBuffers(1, &ebo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.indices.size() * sizeof(u32)),
                     data.indices.data(), GL_STATIC_DRAW);
        indexCount_ = static_cast<GLsizei>(data.indices.size());
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    vertexCount_ = static_cast<GLsizei>(vertices);
    name_ = data.name;
    return true;
}

bool Mesh::createLines(const float* positions, const float* colors, const std::size_t vertexCount) {
    destroy();
    if (!positions || !colors || vertexCount == 0) return false;

    std::vector<float> interleaved(vertexCount * kLineStrideFloats);
    for (std::size_t v = 0; v < vertexCount; ++v) {
        float* out = &interleaved[v * kLineStrideFloats];
        out[0] = positions[v * 3 + 0];
        out[1] = positions[v * 3 + 1];
        out[2] = positions[v * 3 + 2];
        out[3] = colors[v * 3 + 0];
        out[4] = colors[v * 3 + 1];
        out[5] = colors[v * 3 + 2];
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(interleaved.size() * sizeof(float)),
                 interleaved.data(), GL_STATIC_DRAW);
    const GLsizei stride = kLineStrideFloats * static_cast<GLsizei>(sizeof(float));
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    vertexCount_ = static_cast<GLsizei>(vertexCount);
    name_ = "viewport_overlay";
    return true;
}

void Mesh::destroy() {
    if (ebo_) glDeleteBuffers(1, &ebo_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    clearHandles();
    vertexCount_ = 0;
    indexCount_ = 0;
}

void Mesh::drawElements() const {
    drawElements(0, indexCount_);
}

void Mesh::drawElements(const GLsizei firstIndex, const GLsizei count) const {
    if (!vao_ || firstIndex < 0 || count <= 0 || firstIndex >= indexCount_) return;
    const GLsizei clampedCount = std::min(count, indexCount_ - firstIndex);
    if (clampedCount <= 0) return;
    glBindVertexArray(vao_);
    const auto byteOffset = static_cast<std::uintptr_t>(firstIndex) * sizeof(u32);
    glDrawElements(GL_TRIANGLES, clampedCount, GL_UNSIGNED_INT,
                   reinterpret_cast<const void*>(byteOffset));
    glBindVertexArray(0);
}

void Mesh::drawLines(const GLsizei count) const {
    if (!vao_) return;
    const GLsizei n = count < vertexCount_ ? count : vertexCount_;
    if (n <= 0) return;
    glBindVertexArray(vao_);
    glDrawArrays(GL_LINES, 0, n);
    glBindVertexArray(0);
}

} // namespace ember::render
