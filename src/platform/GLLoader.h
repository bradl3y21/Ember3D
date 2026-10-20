// src/platform/GLLoader.h — GL function loading
// Contract: ARCHITECTURE.md §4.2. Owned by Kiln · Platform.
#pragma once

namespace ember::platform {

// Loads all OpenGL 4.5 core entry points via GLAD. The calling thread must
// have a current GL context (Window::create makes one current).
// Returns false if the context exposes less than GL 4.5 core.
bool loadGLFunctions();

// Context info, valid after loadGLFunctions() succeeded.
const char* glVendorString();
const char* glRendererString();
const char* glVersionString();

} // namespace ember::platform
