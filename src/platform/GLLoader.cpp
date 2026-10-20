#include "platform/GLLoader.h"

#include <glad/gl.h>        // GLAD
#include <GLFW/glfw3.h>     // glfwGetProcAddress (GLFW_INCLUDE_NONE is set)
#include <cstdio>

namespace ember::platform {

namespace {
int g_glVersion = 0;

GLADapiproc glfwLoadProc(const char* name) {
    return reinterpret_cast<GLADapiproc>(glfwGetProcAddress(name));
}
} // namespace

bool loadGLFunctions() {
    g_glVersion = gladLoadGL(glfwLoadProc);
    if (g_glVersion == 0) {
        std::fprintf(stderr, "[ember::platform] gladLoadGL failed\n");
        return false;
    }

    const int major = GLAD_VERSION_MAJOR(g_glVersion);
    const int minor = GLAD_VERSION_MINOR(g_glVersion);
    if (major < 4 || (major == 4 && minor < 5)) {
        std::fprintf(stderr,
                     "[ember::platform] OpenGL %d.%d < 4.5 core - not supported\n",
                     major, minor);
        g_glVersion = 0;
        return false;
    }

    std::printf("[ember::platform] OpenGL %s | %s | %s\n",
                glVersionString(), glRendererString(), glVendorString());
    return true;
}

const char* glVendorString() {
    const GLubyte* v = g_glVersion ? glGetString(GL_VENDOR) : nullptr;
    return v ? reinterpret_cast<const char*>(v) : "(no context)";
}

const char* glRendererString() {
    const GLubyte* v = g_glVersion ? glGetString(GL_RENDERER) : nullptr;
    return v ? reinterpret_cast<const char*>(v) : "(no context)";
}

const char* glVersionString() {
    const GLubyte* v = g_glVersion ? glGetString(GL_VERSION) : nullptr;
    return v ? reinterpret_cast<const char*>(v) : "(no context)";
}

} // namespace ember::platform
