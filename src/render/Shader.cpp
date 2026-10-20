// Shader.cpp — program build, use, uniform cache.
#include "render/Shader.h"

#include <glad/gl.h>

#include <string>
#include <vector>

#include "core/Log.h"

namespace ember::render {

Shader::~Shader() {
    destroy();
}

bool Shader::build(const char* vertexGLSL, const char* fragmentGLSL, const char* debugName) {
    destroy();
    name_ = debugName ? debugName : "shader";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertexGLSL, nullptr);
    glCompileShader(vs);
    GLint ok = GL_FALSE;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint len = 0;
        glGetShaderiv(vs, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 1 ? static_cast<size_t>(len) : 1, '\0');
        glGetShaderInfoLog(vs, len, nullptr, log.data());
        const std::string msg = std::string("ember_render: vertex shader failed to compile (") + name_ + "): " + log.data();
        EMBER_LOG_ERROR("%s", msg.c_str());
        glDeleteShader(vs);
        return false;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragmentGLSL, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint len = 0;
        glGetShaderiv(fs, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 1 ? static_cast<size_t>(len) : 1, '\0');
        glGetShaderInfoLog(fs, len, nullptr, log.data());
        const std::string msg = std::string("ember_render: fragment shader failed to compile (") + name_ + "): " + log.data();
        EMBER_LOG_ERROR("%s", msg.c_str());
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 1 ? static_cast<size_t>(len) : 1, '\0');
        glGetProgramInfoLog(program, len, nullptr, log.data());
        const std::string msg = std::string("ember_render: shader program failed to link (") + name_ + "): " + log.data();
        EMBER_LOG_ERROR(msg.c_str());
        glDeleteProgram(program);
        return false;
    }

    programId_ = program;
    uniformCache_.clear();
    {
        const std::string msg = std::string("ember_render: shader built: ") + name_;
        EMBER_LOG_INFO("%s", msg.c_str());
    }
    return true;
}

void Shader::use() const {
    if (programId_) glUseProgram(programId_);
}

void Shader::destroy() {
    if (programId_) {
        glDeleteProgram(programId_);
        programId_ = 0;
    }
    uniformCache_.clear();
}

int Shader::location(const char* name) {
    if (!programId_) return -1;
    auto it = uniformCache_.find(name);
    if (it != uniformCache_.end()) return it->second;
    const GLint loc = glGetUniformLocation(programId_, name);
    uniformCache_.emplace(name, static_cast<int>(loc));
    if (loc < 0) {
        // Cache the miss too — this warns once per name, not once per frame.
        const std::string msg = std::string("ember_render: uniform not found in shader (") + name_ + "): " + name;
        EMBER_LOG_WARN("%s", msg.c_str());
    }
    return static_cast<int>(loc);
}

void Shader::setMat4(const char* name, const Mat4& m) {
    const int loc = location(name);
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, m.m);
}

void Shader::setVec3(const char* name, const float x, const float y, const float z) {
    const int loc = location(name);
    if (loc >= 0) glUniform3f(loc, x, y, z);
}

void Shader::setVec4(const char* name, const float x, const float y, const float z, const float w) {
    const int loc = location(name);
    if (loc >= 0) glUniform4f(loc, x, y, z, w);
}

void Shader::setFloat(const char* name, const float v) {
    const int loc = location(name);
    if (loc >= 0) glUniform1f(loc, v);
}

void Shader::setInt(const char* name, const int v) {
    const int loc = location(name);
    if (loc >= 0) glUniform1i(loc, v);
}

void Shader::setVec3Array(const char* name, const float* data, const int count) {
    const int loc = location(name);
    if (loc >= 0 && count > 0) glUniform3fv(loc, count, data);
}

void Shader::setFloatArray(const char* name, const float* data, const int count) {
    const int loc = location(name);
    if (loc >= 0 && count > 0) glUniform1fv(loc, count, data);
}

void Shader::setIntArray(const char* name, const int* data, const int count) {
    const int loc = location(name);
    if (loc >= 0 && count > 0) glUniform1iv(loc, count, data);
}

} // namespace ember::render
