// Shader.h — GL program compile/link plus a uniform location cache.
//
// Owned by render (ARCHITECTURE.md §2). The cache exists because renderFrame
// sets the same handful of uniforms for every DrawItem; a glGetUniformLocation
// per draw would dominate the stats.
#pragma once

#include <string>
#include <unordered_map>

#include "core/Types.h"

// glad types are what GL calls want; include in the .cpp and keep this header
// light. u32/glId below is a plain program id; core::u32 matches GLuint on MSVC.
namespace ember::render {

class Shader {
public:
    Shader() = default;
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    // Compiles and links a vertex+fragment pair (GLSL source embedded in the
    // renderer — there is no asset loading until the project has one).
    // Returns false and logs on any GL error.
    bool build(const char* vertexGLSL, const char* fragmentGLSL, const char* debugName);

    void use() const;
    void destroy();

    bool valid() const { return programId_ != 0; }

    // Uniform setters — first call per name does the lookup, later ones hit
    // the cache. Safe to call with a missing uniform (ignored, warn once).
    void setMat4(const char* name, const Mat4& m);
    void setVec3(const char* name, float x, float y, float z);
    void setVec4(const char* name, float x, float y, float z, float w);
    void setFloat(const char* name, float v);
    void setInt(const char* name, int v);
    // Arrays (D19 light list): name is the un-suffixed array name ("uLightDir"
    // binds element 0; GLSL guarantees consecutive locations for arrays).
    void setVec3Array(const char* name, const float* data, int count);
    void setFloatArray(const char* name, const float* data, int count);
    void setIntArray(const char* name, const int* data, int count);

private:
    int location(const char* name);   // cached glGetUniformLocation; -1 = absent

    unsigned int programId_ = 0;
    std::string name_;
    std::unordered_map<std::string, int> uniformCache_;
};

} // namespace ember::render
